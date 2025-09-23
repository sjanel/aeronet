#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "socket.hpp"
#include "test_server_fixture.hpp"

using namespace aeronet;

// Helper to connect to localhost:port returning fd or -1.
namespace {
Socket connectLoopback(uint16_t port) {
  Socket sock(Socket::Type::STREAM);
  int fd = sock.fd();
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    return {};  // return empty moved-from socket (fd becomes -1)
  }
  return sock;
}
}  // anonymous namespace

TEST(HttpHeaderTimeout, SlowHeadersConnectionClosed) {
  ServerConfig cfg;
  cfg.withPort(0).withHeaderReadTimeout(std::chrono::milliseconds(50));
  TestServer ts(cfg);
  ts.server.setHandler([](const HttpRequest&) {
    HttpResponse resp{200, "OK"};
    resp.body = "hi";
    resp.contentType = "text/plain";
    return resp;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  Socket sock = connectLoopback(ts.port());
  int fd = sock.fd();
  ASSERT_GE(fd, 0) << "connect failed";
  // Send only method token slowly
  const char* part1 = "GET /";  // incomplete, no version yet
  ASSERT_GT(::send(fd, part1, std::strlen(part1), 0), 0) << strerror(errno);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));  // exceed 50ms timeout
  // Attempt to finish request
  const char* rest = " HTTP/1.1\r\nHost: x\r\n\r\n";
  [[maybe_unused]] auto sent = ::send(fd, rest, std::strlen(rest), 0);
  // Kernel may still accept bytes, but server should close shortly after detecting timeout.

  // Attempt to read response; expect either 0 (closed) or nothing meaningful (no 200 OK)
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  char buf[256];
  auto rcv = ::recv(fd, buf, sizeof(buf), 0);
  // If timeout occurred before headers complete, server closes without response (rcv==0) or ECONNRESET
  if (rcv > 0) {
    std::string_view sv(buf, static_cast<std::string_view::size_type>(rcv));
    // Should not have produced a 200 OK response because headers were never completed before timeout
    EXPECT_EQ(sv.find("200 OK"), std::string_view::npos) << sv;
  }
  ts.stop();
}
