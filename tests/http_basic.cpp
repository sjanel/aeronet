#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"

using namespace std::chrono_literals;

namespace {
std::string httpGet(uint16_t port, const std::string& target, const std::string& extraHeaders = {}) {
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
  std::string req = "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nX-Test: abc123\r\n" + extraHeaders +
                    "Connection: close\r\n\r\n";
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

TEST(HttpBasic, SimpleGet) {
  std::atomic_bool stop{false};
  aeronet::HttpServer server(aeronet::ServerConfig{});  // ephemeral
  auto port = server.port();
  server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    auto testHeaderIt = req.headers.find("X-Test");
    resp.body = std::string("You requested: ") + std::string(req.target);
    if (testHeaderIt != req.headers.end() && !testHeaderIt->second.empty()) {
      resp.body += ", X-Test=";
      resp.body.append(testHeaderIt->second);
    }
    return resp;
  });
  std::jthread th([&] { server.runUntil([&] { return stop.load(); }, 50ms); });
  // Give server time to start
  std::this_thread::sleep_for(100ms);
  std::string resp = httpGet(port, "/abc");
  stop.store(true);
  th.join();
  ASSERT_FALSE(resp.empty());
  ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
  ASSERT_NE(std::string::npos, resp.find("You requested: /abc"));
  ASSERT_NE(std::string::npos, resp.find("X-Test=abc123"));
}
