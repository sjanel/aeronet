#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

#include "aeronet/http-server-config.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/single-http-server.hpp"

namespace aeronet::test {

// Lightweight RAII test server harness to reduce boilerplate in unit tests.
// Responsibilities:
//  * Construct SingleHttpServer (binds & listens immediately)
//  * Start event loop in a background jthread using runUntil(stopFlag)
//  * Provide simple readiness wait (loopback connect) instead of arbitrary sleep_for
//  * Stop & join automatically on destruction (idempotent)
//
// Usage pattern:
//   TestServer ts(HttpServerConfig{});              // starts immediately
//   ts.router().setDefault(...);                  // register handlers
//   auto port = ts.port();                      // fetch ephemeral port
//   <perform requests>
//   // automatic cleanup at scope end (or call ts.stop() early)
//
// Thread-safety: same as underlying SingleHttpServer (single-threaded event loop). Do not call stop() concurrently
// from multiple threads (benign but unnecessary).

class TestServer {
 public:
  explicit TestServer(aeronet::HttpServerConfig cfg, aeronet::RouterConfig routerCfg = {},
                      std::chrono::milliseconds pollPeriod = std::chrono::milliseconds{1});

  [[nodiscard]] uint16_t port() const { return server.port(); }

  void postConfigUpdate(std::function<void(HttpServerConfig&)> updater);

  void postRouterUpdate(std::function<void(Router&)> updater);

  RouterUpdateProxy router() { return server.router(); }

  RouterUpdateProxy resetRouterAndGet(std::function<void(Router&)> initializer = {});

  // Cooperative stop; safe to call multiple times.
  void stop() { server.stop(); }

  void resetRouter(std::function<void(Router&)> initializer = {});

  SingleHttpServer server;

 private:
  void waitReady(std::chrono::milliseconds timeout) const;
};

bool WaitForServer(SingleHttpServer& server, bool running = true,
                   std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

}  // namespace aeronet::test