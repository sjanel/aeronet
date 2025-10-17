#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

namespace {
std::string collectSimple(uint16_t port, std::string_view req) {
  aeronet::test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  EXPECT_GE(fd, 0);
  EXPECT_TRUE(aeronet::test::sendAll(fd, req));
  return aeronet::test::recvUntilClosed(fd);
}
}  // namespace

TEST(Http10, BasicVersionEcho) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse respObj;
    respObj.body("A");
    return respObj;
  });
  std::string req = "GET /x HTTP/1.0\r\nHost: h\r\n\r\n";
  std::string resp = collectSimple(ts.port(), req);
  ASSERT_TRUE(resp.contains("HTTP/1.0 200"));
}

TEST(Http10, No100ContinueEvenIfHeaderPresent) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse respObj;
    respObj.body("B");
    return respObj;
  });
  // Expect ignored in HTTP/1.0
  std::string req =
      "POST /p HTTP/1.0\r\nHost: h\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  std::string resp = collectSimple(ts.port(), req);
  ASSERT_FALSE(resp.contains("100 Continue"));
  ASSERT_TRUE(resp.contains("HTTP/1.0 200"));
}

TEST(Http10, RejectTransferEncoding) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body("C");
    return respObj;
  });
  std::string req = "GET /te HTTP/1.0\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  std::string resp = collectSimple(ts.port(), req);
  // Should return 400 per implementation decision
  ASSERT_TRUE(resp.contains("400"));
}

TEST(Http10, KeepAliveOptInStillWorks) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse respObj;
    respObj.body("D");
    return respObj;
  });
  aeronet::test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req1 = "GET /k1 HTTP/1.0\r\nHost: h\r\nConnection: keep-alive\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, req1));
  std::string first = aeronet::test::recvWithTimeout(fd, 300ms);
  ASSERT_TRUE(first.contains("HTTP/1.0 200"));
  ASSERT_TRUE(first.contains("Connection: keep-alive"));
  std::string req2 = "GET /k2 HTTP/1.0\r\nHost: h\r\nConnection: keep-alive\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, req2));
  std::string second = aeronet::test::recvWithTimeout(fd, 300ms);
  ASSERT_TRUE(second.contains("HTTP/1.0 200"));
}
