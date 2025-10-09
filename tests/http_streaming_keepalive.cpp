#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

TEST(StreamingKeepAlive, TwoSequentialRequests) {
  HttpServerConfig cfg;
  cfg.reusePort = false;
  cfg.enableKeepAlive = true;
  HttpServer server(cfg);
  server.setStreamingHandler([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.write("hello");
    writer.write(",world");
    writer.end();
  });
  std::jthread th([&] { server.run(); });
  auto port = server.port();
  ASSERT_GT(port, 0);
  ASSERT_LE(port, 65535);
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();
  std::string req1 = "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  aeronet::test::sendAll(fd, req1);
  auto r1 = aeronet::test::recvWithTimeout(fd);
  ASSERT_FALSE(r1.empty());
  // Send second request on same connection.
  std::string req2 = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";  // request close after second
  aeronet::test::sendAll(fd, req2);
  auto r2 = aeronet::test::recvWithTimeout(fd);
  ASSERT_FALSE(r2.empty());
  server.stop();
}

TEST(StreamingKeepAlive, HeadRequestReuse) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  HttpServer server(cfg);
  server.setStreamingHandler([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.write("ignored-body");
    writer.end();
  });
  std::jthread th([&] { server.run(); });
  auto port = server.port();
  ASSERT_GT(port, 0);
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();

  std::string hreq = "HEAD / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  aeronet::test::sendAll(fd, hreq);
  auto hr = aeronet::test::recvWithTimeout(fd);
  // Ensure no body appears after header terminator.
  auto pos = hr.find(aeronet::http::DoubleCRLF);
  ASSERT_NE(pos, std::string::npos);
  ASSERT_TRUE(hr.substr(pos + aeronet::http::DoubleCRLF.size()).empty());
  // second GET
  std::string g2 = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  aeronet::test::sendAll(fd, g2);
  auto gr2 = aeronet::test::recvWithTimeout(fd);
  ASSERT_NE(gr2.find("ignored-body"), std::string::npos);  // ensure body from second request present
  server.stop();
}
