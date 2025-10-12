#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;  // retained for potential future literal use

// New tests: pipelining, zero-length Expect (no 100), maxRequestsPerConnection, pipeline error after success, 413 via
// large Content-Length

TEST(HttpPipeline, TwoRequestsBackToBack) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse respObj;
    respObj.body(std::string("E:") + std::string(req.path()));
    return respObj;
  });
  aeronet::test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string combo =
      "GET /a HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\nContent-Length: "
      "0\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, combo));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ASSERT_NE(std::string::npos, resp.find("E:/a"));
  ASSERT_NE(std::string::npos, resp.find("E:/b"));
}

TEST(HttpExpect, ZeroLengthNo100) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body("Z");
    return respObj;
  });
  aeronet::test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string headers =
      "POST /z HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, headers));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ASSERT_EQ(std::string::npos, resp.find("100 Continue"));
  ASSERT_NE(std::string::npos, resp.find('Z'));
}

TEST(HttpMaxRequests, CloseAfterLimit) {
  aeronet::HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body("Q");
    return respObj;
  });
  aeronet::test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string reqs =
      "GET /1 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nGET /2 HTTP/1.1\r\nHost: x\r\nContent-Length: "
      "0\r\n\r\nGET /3 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, reqs));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ASSERT_EQ(2, aeronet::test::countOccurrences(resp, "HTTP/1.1 200"));
  ASSERT_EQ(2, aeronet::test::countOccurrences(resp, "Q"));
}

TEST(HttpPipeline, SecondMalformedAfterSuccess) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body("OK");
    return respObj;
  });
  aeronet::test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string piped = "GET /good HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nBADSECONDREQUEST\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, piped));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ASSERT_NE(std::string::npos, resp.find("OK"));
  ASSERT_NE(std::string::npos, resp.find("400"));
}

TEST(HttpContentLength, ExplicitTooLarge413) {
  aeronet::HttpServerConfig cfg;
  cfg.withMaxBodyBytes(10);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body("R");
    return respObj;
  });
  aeronet::test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "POST /big HTTP/1.1\r\nHost: x\r\nContent-Length: 20\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, req));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ASSERT_NE(std::string::npos, resp.find("413"));
}

TEST(HttpContentLength, GlobalHeaders) {
  aeronet::HttpServerConfig cfg;
  cfg.globalHeaders.emplace_back("X-Global", "gvalue");
  cfg.globalHeaders.emplace_back("X-Another", "anothervalue");
  cfg.globalHeaders.emplace_back("X-Custom", "global");  // overridden by handler
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.customHeader("X-Custom", "original");
    respObj.body("R");
    return respObj;
  });
  aeronet::test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "POST /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, req));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  EXPECT_NE(std::string::npos, resp.find("\r\nX-Global: gvalue"));
  EXPECT_NE(std::string::npos, resp.find("\r\nX-Another: anothervalue"));
  EXPECT_NE(std::string::npos, resp.find("\r\nX-Custom: original"));
}

TEST(HttpBasic, LargePayload) {
  std::string largeBody(1 << 24, 'a');
  aeronet::HttpServerConfig cfg;
  cfg.maxOutboundBufferBytes = largeBody.size() + 512;  // +512 for headers
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setDefault([&largeBody](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body(largeBody);
    return respObj;
  });
  aeronet::test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "GET /good HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, req));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200"));
  EXPECT_TRUE(resp.contains(largeBody));
}

TEST(HttpBasic, ManyHeadersRequest) {
  // Test handling a request with thousands of headers
  aeronet::HttpServerConfig cfg;
  cfg.withMaxHeaderBytes(128UL * 1024UL);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setDefault([](const aeronet::HttpRequest& req) {
    int headerCount = 0;
    for (const auto& [key, value] : req.headers()) {
      if (key.starts_with("X-Custom-")) {
        ++headerCount;
      }
    }
    aeronet::HttpResponse respObj;
    respObj.body("Received " + std::to_string(headerCount) + " custom headers");
    return respObj;
  });
  aeronet::test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  // Build request with many custom headers
  static constexpr int kNbHeaders = 3000;
  std::string req = "GET /test HTTP/1.1\r\nHost: localhost\r\n";
  for (int headerPos = 0; headerPos < kNbHeaders; ++headerPos) {
    req += "X-Custom-" + std::to_string(headerPos) + ": value" + std::to_string(headerPos) + "\r\n";
  }
  req += "Content-Length: 0\r\nConnection: close\r\n\r\n";

  ASSERT_TRUE(aeronet::test::sendAll(fd, req));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200"));
  EXPECT_TRUE(resp.contains("Received " + std::to_string(kNbHeaders) + " custom headers"));
}

TEST(HttpBasic, ManyHeadersResponse) {
  // Test generating a response with thousands of headers
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    // Add 3000 custom headers to response
    for (int i = 0; i < 3000; ++i) {
      respObj.addCustomHeader("X-Response-" + std::to_string(i), "value" + std::to_string(i));
    }
    respObj.body("Response with many headers");
    return respObj;
  });
  aeronet::test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  std::string req = "GET /test HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, req));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200"));
  EXPECT_TRUE(resp.contains("Response with many headers"));

  // Verify some of the custom headers are present
  EXPECT_TRUE(resp.contains("X-Response-0: value0"));
  EXPECT_TRUE(resp.contains("X-Response-500: value500"));
  EXPECT_TRUE(resp.contains("X-Response-999: value999"));
  EXPECT_TRUE(resp.contains("X-Response-1499: value1499"));
  EXPECT_TRUE(resp.contains("X-Response-1999: value1999"));
  EXPECT_TRUE(resp.contains("X-Response-2999: value2999"));
}