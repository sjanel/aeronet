// Tests for custom header forwarding and reserved header protection
#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "test_server_fixture.hpp"
#include "test_util.hpp"

using namespace std::chrono_literals;

TEST(HttpHeadersCustom, ForwardsSingleAndMultipleCustomHeaders) {
  TestServer ts(aeronet::HttpServerConfig{});
  ts.server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse r;
    r.statusCode(201).reason("Created");
    r.header("X-One", "1");
    r.header("X-Two", "two");
    r.contentType("text/plain");
    r.body("B");
    return r;
  });
  ClientConnection cc(ts.port());
  int fd = cc.fd();
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  tu_sendAll(fd, req);
  std::string resp = tu_recvUntilClosed(fd);
  ASSERT_NE(std::string::npos, resp.find("201 Created"));
  ASSERT_NE(std::string::npos, resp.find("X-One: 1"));
  ASSERT_NE(std::string::npos, resp.find("X-Two: two"));
  ASSERT_NE(std::string::npos, resp.find("Content-Length: 1"));  // auto generated
  ASSERT_NE(std::string::npos, resp.find("Connection:"));        // auto generated (keep-alive or close)
}

#ifdef NDEBUG
// In release builds assertions are disabled; just ensure we can set non-reserved but not crash when attempting what
// would be reserved (we avoid actually invoking UB). This block left empty intentionally.
#else
TEST(HttpHeadersCustom, SettingReservedHeaderTriggersAssert) {
  // We use EXPECT_DEATH to verify debug assertion fires when user attempts to set reserved headers.
  TestServer ts(aeronet::HttpServerConfig{});
  // Connection
  ASSERT_DEATH(
      {
        aeronet::HttpResponse r;
        r.header("Connection", "keep-alive");
      },
      "");
  // Date
  ASSERT_DEATH(
      {
        aeronet::HttpResponse r;
        r.header("Date", "Wed, 01 Jan 2020 00:00:00 GMT");
      },
      "");
  // Content-Length
  ASSERT_DEATH(
      {
        aeronet::HttpResponse r;
        r.header("Content-Length", "10");
      },
      "");
  // Transfer-Encoding
  ASSERT_DEATH(
      {
        aeronet::HttpResponse r;
        r.header("Transfer-Encoding", "chunked");
      },
      "");
}
#endif

TEST(HttpHeadersCustom, LocationHeaderAllowed) {
  TestServer ts(aeronet::HttpServerConfig{});
  ts.server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse r;
    r.statusCode(302).reason("Found").location("/new").body("");
    return r;
  });
  ClientConnection cc(ts.port());
  int fd = cc.fd();
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  tu_sendAll(fd, req);
  std::string resp = tu_recvUntilClosed(fd);
  ASSERT_NE(std::string::npos, resp.find("302 Found"));
  ASSERT_NE(std::string::npos, resp.find("Location: /new"));
}
