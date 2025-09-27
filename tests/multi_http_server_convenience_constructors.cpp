#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/multi-http-server.hpp"
#include "invalid_argument_exception.hpp"
#include "socket.hpp"

namespace {
std::string simpleGet(uint16_t port, const char* path = "/") {
  aeronet::Socket sock(aeronet::Socket::Type::STREAM);
  int fd = sock.fd();
  if (fd < 0) {
    return {};
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    return {};
  }
  std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  std::string out;
  char buf[2048];
  while (true) {
    auto bytesRead = ::recv(fd, buf, sizeof(buf), 0);
    if (bytesRead > 0) {
      out.append(buf, buf + bytesRead);
      continue;
    }
    break;  // bytesRead == 0 (EOF) or error -> stop
  }
  return out;
}
}  // namespace

// 1. Auto thread-count constructor
TEST(MultiHttpServer, AutoThreadCountConstructor) {
  aeronet::HttpServerConfig cfg;
  aeronet::MultiHttpServer multi(cfg);
  multi.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("Auto");
    return resp;
  });
  multi.start();
  auto port = multi.port();
  ASSERT_GT(port, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  auto resp = simpleGet(port);
  EXPECT_NE(std::string::npos, resp.find("Auto"));
  auto stats = multi.stats();
  EXPECT_GE(stats.per.size(), static_cast<std::size_t>(1));
  multi.stop();
  EXPECT_FALSE(multi.isRunning());
}

// 2. Explicit thread-count constructor
TEST(MultiHttpServer, ExplicitThreadCountConstructor) {
  aeronet::HttpServerConfig cfg;
  cfg.port = 0;
  cfg.reusePort = true;  // explicit reusePort
  const uint32_t threads = 2;
  aeronet::MultiHttpServer multi(cfg, threads);
  multi.setHandler([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body("Explicit");
    return resp;
  });
  multi.start();
  ASSERT_GT(multi.port(), 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  auto resp = simpleGet(multi.port(), "/exp");
  EXPECT_NE(std::string::npos, resp.find("Explicit"));
  auto stats = multi.stats();
  EXPECT_EQ(stats.per.size(), static_cast<std::size_t>(threads));
  multi.stop();
}

// 3. Move construction (move underlying servers ownership)
TEST(MultiHttpServer, MoveConstruction) {
  aeronet::HttpServerConfig cfg;
  cfg.port = 0;
  aeronet::MultiHttpServer original(cfg);  // auto threads
  original.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("Move");
    return resp;
  });
  original.start();
  auto port = original.port();
  ASSERT_GT(port, 0);
  // Move into new instance
  aeronet::MultiHttpServer moved(std::move(original));
  // Original should no longer be running (state moved)
  EXPECT_FALSE(moved.port() == 0);
  // Basic request still works after move
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  auto resp = simpleGet(moved.port(), "/mv");
  EXPECT_NE(std::string::npos, resp.find("Move"));
  moved.stop();
}

// 4. Invalid thread-count explicit constructor (compile-time / runtime guard)
TEST(MultiHttpServer, InvalidExplicitThreadCountThrows) {
  aeronet::HttpServerConfig cfg;
  cfg.port = 0;
  EXPECT_THROW(aeronet::MultiHttpServer(cfg, 0), aeronet::invalid_argument);  // 0 illegal here
}

// 5. Default constructor + move assignment
// Verifies that a default constructed MultiHttpServer is inert (no threads/servers, not running, port=0)
// and that move-assigning an active instance into it transfers ownership correctly while leaving the
// source in a safe, inert state.
TEST(MultiHttpServer, DefaultConstructorAndMoveAssignment) {
  // Prepare a running instance (rhs of move assignment)
  aeronet::HttpServerConfig cfg;
  cfg.port = 0;                           // ephemeral
  aeronet::MultiHttpServer running(cfg);  // auto thread count
  running.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("MoveAssign");
    return resp;
  });
  running.start();
  ASSERT_TRUE(running.isRunning());
  ASSERT_GT(running.port(), 0);
  const auto originalPort = running.port();
  const auto originalThreads = running.nbThreads();
  ASSERT_GE(originalThreads, 1U);

  // Default constructed target (lhs of move assignment)
  aeronet::MultiHttpServer target;  // default ctor (inert)
  EXPECT_FALSE(target.isRunning());
  EXPECT_EQ(target.port(), 0);
  EXPECT_EQ(target.nbThreads(), 0U);

  // Move assign
  target = std::move(running);

  // After move assignment: target has adopted state
  EXPECT_TRUE(target.isRunning());
  EXPECT_EQ(target.port(), originalPort);
  EXPECT_EQ(target.nbThreads(), originalThreads);

  // Basic request still works via the moved-to target
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  auto resp = simpleGet(target.port(), "/ma");
  EXPECT_NE(std::string::npos, resp.find("MoveAssign"));

  target.stop();
  EXPECT_FALSE(target.isRunning());
}
