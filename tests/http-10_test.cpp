#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

namespace {
test::TestServer ts(HttpServerConfig{});
}  // namespace

TEST(Http10, BasicVersionEcho) {
  ts.server.router().setDefault([]([[maybe_unused]] const HttpRequest& req) {
    HttpResponse respObj;
    respObj.body("A");
    return respObj;
  });
  std::string req = "GET /x HTTP/1.0\r\nHost: h\r\n\r\n";
  std::string resp = test::sendAndCollect(ts.port(), req);
  ASSERT_TRUE(resp.contains("HTTP/1.0 200"));
}

TEST(Http10, No100ContinueEvenIfHeaderPresent) {
  ts.server.router().setDefault([]([[maybe_unused]] const HttpRequest& req) {
    HttpResponse respObj;
    respObj.body("B");
    return respObj;
  });
  // Expect ignored in HTTP/1.0
  std::string req =
      "POST /p HTTP/1.0\r\nHost: h\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  std::string resp = test::sendAndCollect(ts.port(), req);
  ASSERT_FALSE(resp.contains("100 Continue"));
  ASSERT_TRUE(resp.contains("HTTP/1.0 200"));
}

TEST(Http10, RejectTransferEncoding) {
  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    respObj.body("C");
    return respObj;
  });
  std::string req = "GET /te HTTP/1.0\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  std::string resp = test::sendAndCollect(ts.port(), req);
  // Should return 400 per implementation decision
  ASSERT_TRUE(resp.contains("400"));
}

TEST(Http10, KeepAliveOptInStillWorks) {
  ts.server.router().setDefault([]([[maybe_unused]] const HttpRequest& req) {
    HttpResponse respObj;
    respObj.body("D");
    return respObj;
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req1 = "GET /k1 HTTP/1.0\r\nHost: h\r\nConnection: keep-alive\r\n\r\n";
  test::sendAll(fd, req1);
  std::string first = test::recvWithTimeout(fd, 300ms);
  ASSERT_TRUE(first.contains("HTTP/1.0 200"));
  ASSERT_TRUE(first.contains("Connection: keep-alive"));
  std::string req2 = "GET /k2 HTTP/1.0\r\nHost: h\r\nConnection: keep-alive\r\n\r\n";
  test::sendAll(fd, req2);
  std::string second = test::recvWithTimeout(fd, 300ms);
  ASSERT_TRUE(second.contains("HTTP/1.0 200"));
}
