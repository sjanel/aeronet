#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

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
  explicit TestServer(HttpServerConfig cfg, RouterConfig routerCfg = {},
                      std::chrono::milliseconds poll = std::chrono::milliseconds{1});

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

// Generic RAII scope guard for temporarily changing an HttpServerConfig field via
// TestServer::postConfigUpdate(). Captures the previous value on construction and
// restores it on destruction. Movable so it can be returned from factory functions.
//
// Usage:
//   ScopedConfigUpdate<std::chrono::milliseconds> guard(
//       ts,
//       [](const HttpServerConfig& c) { return c.headerReadTimeout; },
//       [](HttpServerConfig& c, auto v) { c.withHeaderReadTimeout(v); },
//       std::chrono::milliseconds{500});
//
template <typename T>
class ScopedConfigUpdate {
 public:
  template <typename Getter, typename Setter>
  ScopedConfigUpdate(TestServer& server, Getter&& getter, Setter&& setter, T newValue)
      : _server(&server), _setter(std::forward<Setter>(setter)), _previous(getter(server.server.config())) {
    _server->postConfigUpdate([this, val = std::move(newValue)](HttpServerConfig& cfg) { _setter(cfg, val); });
  }

  ScopedConfigUpdate(const ScopedConfigUpdate&) = delete;
  ScopedConfigUpdate& operator=(const ScopedConfigUpdate&) = delete;
  ScopedConfigUpdate(ScopedConfigUpdate&& other) noexcept
      : _server(other._server), _setter(std::move(other._setter)), _previous(std::move(other._previous)) {
    other._server = nullptr;
  }
  ScopedConfigUpdate& operator=(ScopedConfigUpdate&&) = delete;

  ~ScopedConfigUpdate() {
    if (_server != nullptr) {
      try {
        _server->postConfigUpdate([this](HttpServerConfig& cfg) { _setter(cfg, _previous); });
      } catch (...) {
        log::error("Failed to restore HttpServerConfig field in ScopedConfigUpdate destructor");
      }
    }
  }

 private:
  TestServer* _server;
  std::function<void(HttpServerConfig&, T)> _setter;
  T _previous;
};

bool WaitForServer(SingleHttpServer& server, bool running = true,
                   std::chrono::milliseconds timeout = std::chrono::milliseconds{500});

}  // namespace aeronet::test