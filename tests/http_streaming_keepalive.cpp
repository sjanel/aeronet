#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <bits/types/struct_timeval.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

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

namespace {
std::string recvAll(int fd, int timeoutMs = 2000) {
  std::string out;
  char buf[4096];
  struct timeval tv{timeoutMs / 1000, static_cast<long>((timeoutMs % 1000) * 1000)};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  while (true) {
    auto nread = ::recv(fd, buf, sizeof(buf), 0);
    if (nread <= 0) {
      break;
    }
    out.append(buf, buf + nread);
  }
  return out;
}
}  // namespace

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
  ASSERT_EQ(::send(fd, req1.data(), req1.size(), 0), req1.size());
  auto r1 = recvAll(fd);
  ASSERT_FALSE(r1.empty());
  // Send second request on same connection.
  std::string req2 = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";  // request close after second
  ASSERT_EQ(::send(fd, req2.data(), req2.size(), 0), req2.size());
  auto r2 = recvAll(fd);
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
  ASSERT_EQ(::send(fd, hreq.data(), hreq.size(), 0), (ssize_t)hreq.size());
  auto hr = recvAll(fd);
  // Ensure no body appears after header terminator.
  auto pos = hr.find(aeronet::http::DoubleCRLF);
  ASSERT_NE(pos, std::string::npos);
  ASSERT_TRUE(hr.substr(pos + aeronet::http::DoubleCRLF.size()).empty());
  // second GET
  std::string g2 = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  ASSERT_EQ(::send(fd, g2.data(), g2.size(), 0), (ssize_t)g2.size());
  auto gr2 = recvAll(fd);
  ASSERT_NE(gr2.find("ignored-body"), std::string::npos);  // ensure body from second request present
  server.stop();
}
