#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <string_view>

#include "aeronet/async-http-server.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

TEST(StreamingKeepAlive, TwoSequentialRequests) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  cfg.pollInterval = std::chrono::milliseconds(5);
  AsyncHttpServer server(cfg);
  server.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.writeBody("hello");
    writer.writeBody(",world");
    writer.end();
  });

  server.start();

  auto port = server.port();
  ASSERT_NE(port, 0);

  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();
  std::string req1 = "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, req1));
  auto r1 = aeronet::test::recvWithTimeout(fd);
  ASSERT_FALSE(r1.empty());
  // Send second request on same connection.
  std::string req2 = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";  // request close after second
  EXPECT_TRUE(aeronet::test::sendAll(fd, req2));
  auto r2 = aeronet::test::recvWithTimeout(fd);
  ASSERT_FALSE(r2.empty());
}

TEST(StreamingKeepAlive, HeadRequestReuse) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  cfg.pollInterval = std::chrono::milliseconds(5);
  AsyncHttpServer server(cfg);
  server.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.writeBody("ignored-body");
    writer.end();
  });
  server.start();

  auto port = server.port();
  ASSERT_GT(port, 0);
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();

  std::string hreq = "HEAD / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, hreq));
  auto hr = aeronet::test::recvWithTimeout(fd);
  // Ensure no body appears after header terminator.
  auto pos = hr.find(aeronet::http::DoubleCRLF);
  ASSERT_NE(pos, std::string::npos);
  ASSERT_TRUE(hr.substr(pos + aeronet::http::DoubleCRLF.size()).empty());
  // second GET
  std::string g2 = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, g2));
  auto gr2 = aeronet::test::recvWithTimeout(fd);
  ASSERT_TRUE(gr2.contains("ignored-body"));  // ensure body from second request present
}
