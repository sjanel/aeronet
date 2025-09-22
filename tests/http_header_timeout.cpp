#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"

using namespace aeronet;

// Helper to connect to localhost:port returning fd or -1.
static int connectLoopback(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

TEST(HttpHeaderTimeout, SlowHeadersConnectionClosed) {
  ServerConfig cfg;
  cfg.withPort(0).withHeaderReadTimeout(std::chrono::milliseconds(50));
  HttpServer server(cfg);
  server.setHandler([](const HttpRequest&) {
    HttpResponse r{200, "OK"};
    r.body = "hi";
    r.contentType = "text/plain";
    return r;
  });
  auto start = std::chrono::steady_clock::now();
  std::atomic<bool> stopFlag{false};
  std::jthread thr([&] { server.runUntil([&] { return stopFlag.load(); }, std::chrono::milliseconds(5)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  int fd = connectLoopback(server.port());
  ASSERT_GE(fd, 0) << "connect failed";
  // Send only method token slowly
  const char* part1 = "GET /";  // incomplete, no version yet
  ASSERT_GT(::send(fd, part1, std::strlen(part1), 0), 0) << strerror(errno);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));  // exceed 50ms timeout
  // Attempt to finish request
  const char* rest = " HTTP/1.1\r\nHost: x\r\n\r\n";
  ssize_t s = ::send(fd, rest, std::strlen(rest), 0);
  // Kernel may still accept bytes, but server should close shortly after detecting timeout.
  (void)s;
  // Attempt to read response; expect either 0 (closed) or nothing meaningful (no 200 OK)
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  char buf[256];
  ssize_t rcv = ::recv(fd, buf, sizeof(buf), 0);
  // If timeout occurred before headers complete, server closes without response (rcv==0) or ECONNRESET
  if (rcv > 0) {
    std::string_view sv(buf, (size_t)rcv);
    // Should not have produced a 200 OK response because headers were never completed before timeout
    EXPECT_EQ(sv.find("200 OK"), std::string_view::npos) << sv;
  }
  ::close(fd);
  stopFlag.store(true);
  thr.join();
}
