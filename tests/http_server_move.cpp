#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
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
    ssize_t nbRead = ::recv(fd, buf, sizeof(buf), 0);
    if (nbRead <= 0) {
      break;
    }
    out.append(buf, buf + nbRead);
  }
  ::close(fd);
  return out;
}
}  // namespace

TEST(HttpServerMove, MoveConstructAndServe) {
  std::atomic_bool stop{false};
  aeronet::HttpServer original(aeronet::ServerConfig{});
  auto port = original.port();
  original.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = std::string("ORIG:") + std::string(req.target);
    return resp;
  });

  // Move construct server before running
  aeronet::HttpServer moved(std::move(original));

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }, 50ms); });
  std::this_thread::sleep_for(100ms);
  std::string resp = simpleGet(port, "/mv");
  stop.store(true);
  th.join();

  ASSERT_NE(std::string::npos, resp.find("ORIG:/mv"));
}

TEST(HttpServerMove, MoveAssignWhileStopped) {
  aeronet::HttpServer s1(aeronet::ServerConfig{}.withReusePort(false));
  aeronet::HttpServer s2(aeronet::ServerConfig{}.withReusePort(false));
  auto port1 = s1.port();
  auto port2 = s2.port();

  EXPECT_NE(port1, port2);

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
  EXPECT_EQ(s1.port(), port2);

  std::atomic_bool stop{false};
  std::jthread th([&] { s1.runUntil([&] { return stop.load(); }, 50ms); });
  std::this_thread::sleep_for(120ms);
  std::string resp = simpleGet(port2, "/x");
  stop.store(true);
  th.join();
  ASSERT_NE(std::string::npos, resp.find("S2"));
}
