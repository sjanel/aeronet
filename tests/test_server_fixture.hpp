#pragma once
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"

// Lightweight RAII test server harness to reduce boilerplate in unit tests.
// Responsibilities:
//  * Construct HttpServer (binds & listens immediately)
//  * Start event loop in a background jthread using runUntil(stopFlag)
//  * Provide simple readiness wait (loopback connect) instead of arbitrary sleep_for
//  * Stop & join automatically on destruction (idempotent)
//
// Usage pattern:
//   TestServer ts(HttpServerConfig{});              // starts immediately
//   ts.server.setHandler(...);                  // register handlers
//   auto port = ts.port();                      // fetch ephemeral port
//   <perform requests>
//   // automatic cleanup at scope end (or call ts.stop() early)
//
// Thread-safety: same as underlying HttpServer (single-threaded event loop). Do not call stop() concurrently
// from multiple threads (benign but unnecessary).
struct TestServer {
  explicit TestServer(aeronet::HttpServerConfig cfg,
                      std::chrono::milliseconds pollPeriod = std::chrono::milliseconds{50})
      : server(std::move(cfg.withPollInterval(pollPeriod))),
        loopThread([this] { server.runUntil([&] { return stopFlag.load(); }); }) {
    waitReady(std::chrono::milliseconds{500});
  }

  TestServer(const TestServer&) = delete;
  TestServer(TestServer&&) noexcept = delete;
  TestServer& operator=(const TestServer&) = delete;
  TestServer& operator=(TestServer&&) noexcept = delete;

  ~TestServer() { stop(); }

  [[nodiscard]] uint16_t port() const { return server.port(); }

  // Cooperative stop; safe to call multiple times.
  void stop() {
    if (!stopFlag.exchange(true)) {
      server.stop();
    }
  }

  aeronet::HttpServer server;

 private:
  void waitReady(std::chrono::milliseconds timeout) const {
    // The listening socket is active immediately after server construction; a successful connect
    // simply confirms the OS accepted it. We retry briefly to absorb transient startup latency.
    aeronet::test::ClientConnection cnx(port(), timeout);
  }

  std::jthread loopThread;  // auto-join on destruction
  std::atomic_bool stopFlag{false};
};
