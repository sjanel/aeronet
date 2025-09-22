#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"

using namespace std::chrono_literals;

namespace {
std::string rawHttp(uint16_t port, const std::string& verb, const std::string& target) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return {};
  }
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
  ssize_t bytes;
  while ((bytes = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    out.append(buf, buf + bytes);
  }
  ::close(fd);
  return out;
}
}  // namespace

TEST(HttpStreaming, ChunkedSimple) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  auto port = server.port();
  server.setStreamingHandler([]([[maybe_unused]] const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
    writer.setStatus(200, "OK");
    writer.setContentType("text/plain");
    writer.write("hello ");
    writer.write("world");
    writer.end();
  });
  std::jthread th([&] { server.runUntil([] { return false; }, 50ms); });
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
  auto port = server.port();
  server.setStreamingHandler([]([[maybe_unused]] const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
    writer.setStatus(200, "OK");
    writer.setContentType("text/plain");
    writer.write("ignored body");  // should not be emitted for HEAD
    writer.end();
  });
  std::jthread th([&] { server.runUntil([] { return false; }, 50ms); });
  std::this_thread::sleep_for(100ms);
  std::string resp = rawHttp(port, "HEAD", "/head");
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
  // For HEAD we expect no chunked framing. "0\r\n" alone would falsely match the Content-Length header line
  // ("Content-Length: 0\r\n"). What we really want to assert is that there is no terminating chunk sequence.
  // The terminating chunk in a chunked response would appear as "\r\n0\r\n\r\n" (preceded by the blank line
  // after headers or previous chunk). We also assert absence of Transfer-Encoding: chunked and body payload.
  ASSERT_EQ(std::string::npos, resp.find("\r\n0\r\n\r\n"));
  ASSERT_EQ(std::string::npos, resp.find("Transfer-Encoding: chunked"));
  ASSERT_EQ(std::string::npos, resp.find("ignored body"));
  // Positive check: we do expect a Content-Length: 0 header for HEAD.
  ASSERT_NE(std::string::npos, resp.find("Content-Length: 0\r\n"));
}
