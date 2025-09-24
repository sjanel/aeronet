#pragma once
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"

// Lightweight RAII test server harness to reduce boilerplate in unit tests.
// Responsibilities:
//  * Construct HttpServer (binds & listens immediately)
//  * Start event loop in a background jthread using runUntil(stopFlag)
//  * Provide simple readiness wait (loopback connect) instead of arbitrary sleep_for
//  * Stop & join automatically on destruction (idempotent)
//
// Usage pattern:
//   TestServer ts(ServerConfig{});              // starts immediately
//   ts.server.setHandler(...);                  // register handlers
//   auto port = ts.port();                      // fetch ephemeral port
//   <perform requests>
//   // automatic cleanup at scope end (or call ts.stop() early)
//
// Thread-safety: same as underlying HttpServer (single-threaded event loop). Do not call stop() concurrently
// from multiple threads (benign but unnecessary).
struct TestServer {
  aeronet::HttpServer server;
  std::jthread loopThread;  // auto-join on destruction
  std::atomic_bool stopFlag{false};

  explicit TestServer(aeronet::ServerConfig cfg, std::chrono::milliseconds pollPeriod = std::chrono::milliseconds{50})
      : server(std::move(cfg)) {
    loopThread = std::jthread([this, pollPeriod] { server.runUntil([&] { return stopFlag.load(); }, pollPeriod); });
    waitReady(std::chrono::milliseconds{500});
  }

  uint16_t port() const { return server.port(); }

  // Cooperative stop; safe to call multiple times.
  void stop() {
    if (!stopFlag.exchange(true)) {
      server.stop();
    }
  }

  ~TestServer() { stop(); }

 private:
  void waitReady(std::chrono::milliseconds timeout) {
    // The listening socket is active immediately after server construction; a successful connect
    // simply confirms the OS accepted it. We retry briefly to absorb transient startup latency.
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd >= 0) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port());
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
          ::close(fd);
          return;  // ready
        }
        ::close(fd);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    // If we reach here we treat it as best effort; tests may still proceed (will fail fast if unusable).
  }
};
