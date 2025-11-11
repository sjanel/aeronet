#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/test_util.hpp"
#include "log.hpp"

namespace aeronet::test {

// Lightweight RAII test server harness to reduce boilerplate in unit tests.
// Responsibilities:
//  * Construct HttpServer (binds & listens immediately)
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
// Thread-safety: same as underlying HttpServer (single-threaded event loop). Do not call stop() concurrently
// from multiple threads (benign but unnecessary).

class TestHttpServer : public HttpServer {
 public:
  using HttpServer::HttpServer;
};

struct TestServer {
  explicit TestServer(aeronet::HttpServerConfig cfg, aeronet::RouterConfig routerCfg = {},
                      std::chrono::milliseconds pollPeriod = std::chrono::milliseconds{5})
      : server(std::move(cfg.withPollInterval(pollPeriod)), std::move(routerCfg)),
        loopThread([this] { server.runUntil([&] { return stopFlag.load(); }); }) {
    waitReady(std::chrono::milliseconds{500});
  }

  TestServer(const TestServer&) = delete;
  TestServer(TestServer&&) noexcept = delete;
  TestServer& operator=(const TestServer&) = delete;
  TestServer& operator=(TestServer&&) noexcept = delete;

  ~TestServer() { stop(); }

  [[nodiscard]] uint16_t port() const { return server.port(); }

  void postConfigUpdate(std::function<void(HttpServerConfig&)> updater) {
    server.postConfigUpdate(std::move(updater));
    std::this_thread::sleep_for(server.config().pollInterval + 100us);  // allow event loop to process update
  }

  void postRouterUpdate(std::function<void(Router&)> updater) {
    server.postRouterUpdate(std::move(updater));
    std::this_thread::sleep_for(server.config().pollInterval + 100us);
  }

  RouterUpdateProxy router() { return server.router(); }

  RouterUpdateProxy resetRouterAndGet(std::function<void(Router&)> initializer = {}) {
    resetRouter(std::move(initializer));
    return router();
  }

  // Cooperative stop; safe to call multiple times.
  void stop() {
    if (!stopFlag.exchange(true)) {
      server.stop();
    }
  }

  void resetRouter(std::function<void(Router&)> initializer = {}) {
    postRouterUpdate([init = std::move(initializer)](Router& router) mutable {
      router.clear();
      if (init) {
        init(router);
      }
    });
  }

  TestHttpServer server;

 private:
  void waitReady(std::chrono::milliseconds timeout) const {
    // If builtin probes are enabled, actively poll the readiness probe path until we receive 200 OK
    // or the timeout elapses. Otherwise fall back to the simple connect check used previously.
    const auto& cfg = server.config();
    if (cfg.builtinProbes.enabled) {
      const auto probePath = std::string(cfg.builtinProbes.readinessPath());
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        try {
          RequestOptions opt;
          opt.target = probePath;
          auto resp = requestOrThrow(port(), opt);
          // Request returned; check if it contains HTTP/ status 200 in the status line.
          if (resp.contains("HTTP/1.1 200")) {
            return;
          }
        } catch (const std::exception& ex) {
          log::error("Readiness probe request failed, retrying... {}", ex.what());
        }
        std::this_thread::sleep_for(5ms);
      }
      throw std::runtime_error("server readiness probe did not return 200 within timeout");
    }

    // The listening socket is active immediately after server construction; a successful connect
    // simply confirms the OS accepted it. We retry briefly to absorb transient startup latency.
    ClientConnection cnx(port(), timeout);
  }

  std::jthread loopThread;  // auto-join on destruction
  std::atomic_bool stopFlag{false};
};

}  // namespace aeronet::test