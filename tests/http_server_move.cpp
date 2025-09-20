#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "aeronet/server.hpp"

using namespace std::chrono_literals;

namespace {
std::string simpleGet(uint16_t port, const std::string& target) {
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
  std::string req = "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  char buf[4096];
  std::string out;
  while (true) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    out.append(buf, buf + n);
  }
  ::close(fd);
  return out;
}
}  // namespace

TEST(HttpServerMove, MoveConstructAndServe) {
  std::atomic_bool stop{false};
  uint16_t port = 18081;
  aeronet::HttpServer original(port);
  original.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = std::string("ORIG:") + std::string(req.target);
    return resp;
  });

  // Move construct server before running
  aeronet::HttpServer moved(std::move(original));
  EXPECT_EQ(original.port(), 0);  // moved-from reset to 0
  EXPECT_FALSE(original.isRunning());

  std::thread th([&] { moved.runUntil([&] { return stop.load(); }, 50ms); });
  std::this_thread::sleep_for(100ms);
  std::string resp = simpleGet(port, "/mv");
  stop.store(true);
  th.join();

  ASSERT_NE(std::string::npos, resp.find("ORIG:/mv"));
}

TEST(HttpServerMove, MoveAssignWhileStopped) {
  uint16_t port1 = 18082;
  uint16_t port2 = 18083;
  aeronet::HttpServer s1(port1);
  aeronet::HttpServer s2(port2);
  s1.setHandler([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = "S1";
    return resp;
  });
  s2.setHandler([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = "S2";
    return resp;
  });

  // Move assign s1 <- s2 (both stopped)
  s1 = std::move(s2);
  EXPECT_EQ(s2.port(), 0);
  EXPECT_EQ(s1.port(), port2);

  std::atomic_bool stop{false};
  std::thread th([&] { s1.runUntil([&] { return stop.load(); }, 50ms); });
  std::this_thread::sleep_for(120ms);
  std::string resp = simpleGet(port2, "/x");
  stop.store(true);
  th.join();
  ASSERT_NE(std::string::npos, resp.find("S2"));
}
