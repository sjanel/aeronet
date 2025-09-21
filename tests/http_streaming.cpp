#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>

#include "aeronet/server.hpp"

using namespace std::chrono_literals;

namespace {
std::string rawHttp(uint16_t port, const std::string& verb, const std::string& target) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return {};
  }
  std::string req = verb + " " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  char buf[4096];
  std::string out;
  ssize_t n;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    out.append(buf, buf + n);
  }
  ::close(fd);
  return out;
}
}  // namespace

TEST(HttpStreaming, ChunkedSimple) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setStreamingHandler([](const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& w) {
    w.setStatus(200, "OK");
    w.setContentType("text/plain");
    w.write("hello ");
    w.write("world");
    w.end();
  });
  std::thread th([&] { server.runUntil([] { return false; }, 50ms); });
  std::this_thread::sleep_for(100ms);
  std::string resp = rawHttp(port, "GET", "/stream");
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
  // Should contain chunk sizes in hex (6 and 5) and terminating 0 chunk.
  ASSERT_NE(std::string::npos, resp.find("6\r\nhello "));
  ASSERT_NE(std::string::npos, resp.find("5\r\nworld"));
  ASSERT_NE(std::string::npos, resp.find("0\r\n\r\n"));
}

TEST(HttpStreaming, HeadSuppressedBody) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setStreamingHandler([](const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& w) {
    w.setStatus(200, "OK");
    w.setContentType("text/plain");
    w.write("ignored body");  // should not be emitted for HEAD
    w.end();
  });
  std::thread th([&] { server.runUntil([] { return false; }, 50ms); });
  std::this_thread::sleep_for(100ms);
  std::string resp = rawHttp(port, "HEAD", "/head");
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
  // For HEAD we expect no chunk markers or body; only headers and final CRLFCRLF.
  ASSERT_EQ(std::string::npos, resp.find("0\r\n"));
  ASSERT_EQ(std::string::npos, resp.find("ignored body"));
}
