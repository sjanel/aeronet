#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "test_util.hpp"

using namespace std::chrono_literals;  // retained for potential future literal use

// New tests: pipelining, zero-length Expect (no 100), maxRequestsPerConnection, pipeline error after success, 413 via
// large Content-Length

// Introduce TestServer fixture for reducing boilerplate (applied to first test as template for others).
#include "test_server_fixture.hpp"

TEST(HttpPipeline, TwoRequestsBackToBack) {
  TestServer ts(aeronet::ServerConfig{});
  ts.server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse respObj;
    respObj.body = std::string("E:") + std::string(req.target);
    return respObj;
  });
  ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string combo =
      "GET /a HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\nContent-Length: "
      "0\r\nConnection: close\r\n\r\n";
  tu_sendAll(fd, combo);
  std::string resp = tu_recvUntilClosed(fd);
  ASSERT_NE(std::string::npos, resp.find("E:/a"));
  ASSERT_NE(std::string::npos, resp.find("E:/b"));
}

TEST(HttpExpect, ZeroLengthNo100) {
  TestServer ts(aeronet::ServerConfig{});
  ts.server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body = "Z";
    return respObj;
  });
  ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string headers =
      "POST /z HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  tu_sendAll(fd, headers);
  std::string resp = tu_recvUntilClosed(fd);
  ASSERT_EQ(std::string::npos, resp.find("100 Continue"));
  ASSERT_NE(std::string::npos, resp.find('Z'));
}

TEST(HttpMaxRequests, CloseAfterLimit) {
  aeronet::ServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);
  TestServer ts(cfg);
  ts.server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body = "Q";
    return respObj;
  });
  ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string reqs =
      "GET /1 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nGET /2 HTTP/1.1\r\nHost: x\r\nContent-Length: "
      "0\r\n\r\nGET /3 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  tu_sendAll(fd, reqs);
  std::string resp = tu_recvUntilClosed(fd);
  ASSERT_EQ(2, tu_countOccurrences(resp, "HTTP/1.1 200"));
  ASSERT_EQ(2, tu_countOccurrences(resp, "Q"));
}

TEST(HttpPipeline, SecondMalformedAfterSuccess) {
  TestServer ts(aeronet::ServerConfig{});
  ts.server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body = "OK";
    return respObj;
  });
  ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string piped = "GET /good HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nBADSECOND\r\n\r\n";
  tu_sendAll(fd, piped);
  std::string resp = tu_recvUntilClosed(fd);
  ASSERT_NE(std::string::npos, resp.find("OK"));
  ASSERT_NE(std::string::npos, resp.find("400"));
}

TEST(HttpContentLength, ExplicitTooLarge413) {
  aeronet::ServerConfig cfg;
  cfg.withMaxBodyBytes(10);
  TestServer ts(cfg);
  ts.server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body = "R";
    return respObj;
  });
  ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "POST /big HTTP/1.1\r\nHost: x\r\nContent-Length: 20\r\nConnection: close\r\n\r\n";
  tu_sendAll(fd, req);
  std::string resp = tu_recvUntilClosed(fd);
  ASSERT_NE(std::string::npos, resp.find("413"));
}
