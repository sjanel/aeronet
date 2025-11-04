#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "stringconv.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

namespace {
test::TestServer ts(HttpServerConfig{});
}

TEST(HttpPipeline, TwoRequestsBackToBack) {
  ts.server.router().setDefault([](const HttpRequest& req) {
    HttpResponse respObj;
    respObj.body(std::string("E:") + std::string(req.path()));
    return respObj;
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string combo =
      "GET /a HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\nContent-Length: "
      "0\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, combo));
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("E:/a"));
  ASSERT_TRUE(resp.contains("E:/b"));
}

TEST(HttpExpect, ZeroLengthNo100) {
  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body("Z");
    return respObj;
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string headers =
      "POST /z HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, headers));
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_FALSE(resp.contains("100 Continue"));
  ASSERT_TRUE(resp.contains('Z'));
}

TEST(HttpMaxRequests, CloseAfterLimit) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);
  test::TestServer ts(cfg);
  // parser error callback intentionally left empty in tests
  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body("Q");
    return respObj;
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string reqs =
      "GET /1 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nGET /2 HTTP/1.1\r\nHost: x\r\nContent-Length: "
      "0\r\n\r\nGET /3 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, reqs));
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_EQ(2, test::countOccurrences(resp, "HTTP/1.1 200"));
  ASSERT_EQ(2, test::countOccurrences(resp, "Q"));
}

TEST(HttpPipeline, SecondMalformedAfterSuccess) {
  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body("OK");
    return respObj;
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string piped = "GET /good HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nBADSECONDREQUEST\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, piped));
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("OK"));
  ASSERT_TRUE(resp.contains("400"));
}

TEST(HttpContentLength, ExplicitTooLarge413) {
  HttpServerConfig cfg;
  cfg.withMaxBodyBytes(10);
  test::TestServer ts(cfg);
  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body("R");
    return respObj;
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "POST /big HTTP/1.1\r\nHost: x\r\nContent-Length: 20\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, req));
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("413"));
}

TEST(HttpContentLength, GlobalHeaders) {
  HttpServerConfig cfg;
  cfg.globalHeaders.emplace_back("X-Global", "gvalue");
  cfg.globalHeaders.emplace_back("X-Another", "anothervalue");
  cfg.globalHeaders.emplace_back("X-Custom", "global");  // overridden by handler
  test::TestServer ts(cfg);
  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.customHeader("X-Custom", "original");
    respObj.body("R");
    return respObj;
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "POST /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, req));
  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.contains("\r\nX-Global: gvalue"));
  EXPECT_TRUE(resp.contains("\r\nX-Another: anothervalue"));
  EXPECT_TRUE(resp.contains("\r\nX-Custom: original"));
}

TEST(HttpBasic, LargePayload) {
  const std::string largeBody(1 << 24, 'a');
  HttpServerConfig cfg;
  cfg.maxOutboundBufferBytes = largeBody.size() + 512;  // +512 for headers
  test::TestServer ts(cfg);
  ts.server.router().setDefault([&largeBody](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body(largeBody);
    return respObj;
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "GET /good HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, req));
  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200"));
  EXPECT_TRUE(resp.contains(largeBody));
}

TEST(HttpBasic, ManyHeadersRequest) {
  // Test handling a request with thousands of headers
  HttpServerConfig cfg;
  static constexpr std::size_t kMaxHeaderBytes = 128UL * 1024UL;
  cfg.withMaxHeaderBytes(kMaxHeaderBytes);
  test::TestServer ts(cfg);
  ts.server.router().setDefault([](const HttpRequest& req) {
    int headerCount = 0;
    for (const auto& [key, value] : req.headers()) {
      if (key.starts_with("X-Custom-")) {
        ++headerCount;
      }
    }
    HttpResponse respObj;
    respObj.body("Received " + std::to_string(headerCount) + " custom headers");
    return respObj;
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  // Build request with many custom headers
  static constexpr int kNbHeaders = 3000;
  RawChars req(kMaxHeaderBytes);
  req.unchecked_append("GET /test HTTP/1.1\r\nHost: localhost\r\n");
  for (int headerPos = 0; headerPos < kNbHeaders; ++headerPos) {
    req.append("X-Custom-");
    req.append(std::string_view(IntegralToCharVector(headerPos)));
    req.append(": value");
    req.append(std::string_view(IntegralToCharVector(headerPos)));
    req.append(http::CRLF);
  }
  req.append("Content-Length: 0\r\nConnection: close");
  req.append(http::DoubleCRLF);

  ASSERT_TRUE(test::sendAll(fd, req));
  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200"));
  EXPECT_TRUE(resp.contains("Received " + std::to_string(kNbHeaders) + " custom headers"));
}

TEST(HttpBasic, ManyHeadersResponse) {
  // Test generating a response with thousands of headers
  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    // Add 3000 custom headers to response
    for (int i = 0; i < 3000; ++i) {
      respObj.addCustomHeader("X-Response-" + std::to_string(i), "value" + std::to_string(i));
    }
    respObj.body("Response with many headers");
    return respObj;
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  std::string req = "GET /test HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, req));
  std::string resp = test::recvUntilClosed(fd);
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

TEST(HttpExpectation, UnknownExpectationReturns417) {
  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body("X");
    return respObj;
  });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: custom-token\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, req));
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("417")) << resp;
}

TEST(HttpExpectation, MultipleTokensWithUnknownShouldReturn417) {
  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body("X");
    return respObj;
  });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  // Include 100-continue and an unknown token -> RFC requires 417
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue, custom-token\r\nConnection: "
      "close\r\n\r\nHELLO";
  ASSERT_TRUE(test::sendAll(fd, req));
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("417")) << resp;
}

TEST(HttpExpectation, HandlerCanEmit102Interim) {
  // Register handler that emits 102 Processing for token "102-processing"
  ts.server.setExpectationHandler([](const HttpRequest& /*req*/, std::string_view token) {
    HttpServer::ExpectationResult res;
    if (token == "102-processing") {
      res.kind = HttpServer::ExpectationResultKind::Interim;
      res.interimStatus = 102;
      return res;
    }
    res.kind = HttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body("OK");
    return respObj;
  });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 102-processing\r\nConnection: close\r\n\r\nHELLO";
  ASSERT_TRUE(test::sendAll(fd, req));
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("102 Processing")) << resp;
  ASSERT_TRUE(resp.contains("200")) << resp;
}

TEST(HttpExpectation, HandlerInvalidInterimStatusReturns500) {
  // Handler emits an invalid interim status (not 1xx)
  ts.server.setExpectationHandler([](const HttpRequest& /*req*/, std::string_view token) {
    HttpServer::ExpectationResult res;
    if (token == "bad-interim") {
      res.kind = HttpServer::ExpectationResultKind::Interim;
      res.interimStatus = 250;  // invalid: not 1xx
      return res;
    }
    res.kind = HttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body("SHOULD NOT SEE");
    return respObj;
  });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: bad-interim\r\nConnection: close\r\n\r\nHELLO";
  ASSERT_TRUE(test::sendAll(fd, req));
  std::string resp = test::recvUntilClosed(fd);
  // Server should return 500 due to invalid interim status and not invoke handler body
  ASSERT_TRUE(resp.contains("500")) << resp;
  ASSERT_TRUE(resp.contains("Invalid interim status")) << resp;
  ASSERT_FALSE(resp.contains("SHOULD NOT SEE")) << resp;
}

TEST(HttpExpectation, HandlerThrowsReturns500AndSkipsBody) {
  // Handler throws an exception
  ts.server.setExpectationHandler([](const HttpRequest&, std::string_view token) {
    if (token == "throws") {
      throw std::runtime_error("boom");
    }
    HttpServer::ExpectationResult res;
    res.kind = HttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body("SHOULD NOT SEE");
    return respObj;
  });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: throws\r\nConnection: close\r\n\r\nHELLO";
  ASSERT_TRUE(test::sendAll(fd, req));
  std::string resp = test::recvUntilClosed(fd);
  // Server should return 500 due to exception in handler and not invoke handler body
  ASSERT_TRUE(resp.contains("500")) << resp;
  ASSERT_TRUE(resp.contains("Internal Server Error")) << resp;
  ASSERT_FALSE(resp.contains("SHOULD NOT SEE")) << resp;
}

TEST(HttpExpectation, HandlerFinalResponseSkipsBody) {
  // Handler returns a final response immediately
  ts.server.setExpectationHandler([](const HttpRequest& /*req*/, std::string_view token) {
    HttpServer::ExpectationResult res;
    if (token == "auth-check") {
      res.kind = HttpServer::ExpectationResultKind::FinalResponse;
      HttpResponse hr(403, "Forbidden");
      hr.contentType("text/plain").body("nope");
      res.finalResponse = std::move(hr);
      return res;
    }
    res.kind = HttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body("SHOULD NOT SEE");
    return respObj;
  });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: auth-check\r\nConnection: close\r\n\r\nHELLO";
  ASSERT_TRUE(test::sendAll(fd, req));
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("403")) << resp;
  ASSERT_TRUE(resp.contains("nope")) << resp;
  ASSERT_FALSE(resp.contains("SHOULD NOT SEE")) << resp;
}

TEST(HttpExpectation, Mixed100AndCustomWithHandlerContinue) {
  // Handler accepts custom token and returns Continue
  ts.server.setExpectationHandler([](const HttpRequest& /*req*/, std::string_view token) {
    HttpServer::ExpectationResult res;
    if (token == "custom-ok") {
      res.kind = HttpServer::ExpectationResultKind::Continue;
      return res;
    }
    res.kind = HttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body("DONE");
    return respObj;
  });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue, custom-ok\r\nConnection: "
      "close\r\n\r\nHELLO";
  ASSERT_TRUE(test::sendAll(fd, req));
  std::string resp = test::recvUntilClosed(fd);
  // Should see 100 Continue (from expectContinue path) and final 200
  ASSERT_TRUE(resp.contains("100 Continue")) << resp;
  ASSERT_TRUE(resp.contains("200")) << resp;
  ASSERT_TRUE(resp.contains("DONE")) << resp;
}

TEST(HttpHead, MaxRequestsApplied) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(3);
  HttpServer server(cfg);
  auto port = server.port();
  server.router().setDefault([]([[maybe_unused]] const HttpRequest& req) {
    HttpResponse resp;
    resp.body("IGNORED");
    return resp;
  });
  std::jthread th([&] { server.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  // 4 HEAD requests pipelined; only 3 responses expected then close
  std::string reqs;
  for (int i = 0; i < 4; ++i) {
    reqs += "HEAD /h" + std::to_string(i) + " HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  }
  EXPECT_TRUE(test::sendAll(fd, reqs));
  std::string resp = test::recvUntilClosed(fd);
  server.stop();
  int statusCount = 0;
  std::size_t pos = 0;
  while ((pos = resp.find("HTTP/1.1 200", pos)) != std::string::npos) {
    ++statusCount;
    pos += 11;
  }
  ASSERT_EQ(3, statusCount) << resp;
  // HEAD responses must not include body; ensure no accidental body token present
  ASSERT_FALSE(resp.contains("IGNORED"));
}