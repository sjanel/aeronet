#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/multi-http-server.hpp"
#include "aeronet/server-config.hpp"

namespace {
std::string simpleGet(uint16_t port, const char* path) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return {};
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return {};
  }
  std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  std::string buf;
  buf.resize(4096);
  ssize_t received = ::recv(fd, buf.data(), buf.size(), 0);
  if (received > 0) {
    buf.resize(static_cast<std::size_t>(received));
  } else {
    buf.clear();
  }
  ::close(fd);
  return buf;
}
}  // namespace

TEST(MultiHttpServer, BasicStartAndServe) {
  const int threads = 3;
  aeronet::ServerConfig cfg;
  cfg.port = 0;
  cfg.reusePort = true;  // let kernel pick
  aeronet::MultiHttpServer multi(cfg, threads);
  multi.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body = std::string("Hello "); /* path not exposed directly */
    return resp;
  });
  multi.start();
  auto port = multi.port();
  ASSERT_GT(port, 0);
  // allow sockets to be fully listening
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto r1 = simpleGet(port, "/one");
  auto r2 = simpleGet(port, "/two");
  EXPECT_NE(std::string::npos, r1.find("Hello"));
  EXPECT_NE(std::string::npos, r2.find("Hello"));

  auto stats = multi.stats();
  EXPECT_EQ(stats.per.size(), static_cast<std::size_t>(threads));

  multi.stop();
}
