#include "aeronet/test_server_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-server-config.hpp"
#include "aeronet/log-noexcept.hpp"
#include "aeronet/log.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router-update-proxy.hpp"
#include "aeronet/router.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/test_util.hpp"

namespace aeronet::test {

namespace {

void WaitReady(SingleHttpServer& server, std::chrono::milliseconds timeout) noexcept {
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
        const auto resp = requestOrThrow(server.port(), opt);
        // Request returned; check if it contains HTTP/ status 200 in the status line.
        if (resp.starts_with("HTTP/1.1 200")) {
          return;
        }
      } catch (const std::exception& ex) {
        log_noexcept::error("Readiness probe request failed, retrying... {}", ex.what());
      }
      std::this_thread::sleep_for(5ms);  // NOLINT(misc-include-cleaner)
    }
    log_noexcept::error("server readiness probe did not return 200 within timeout");
  }

  try {
    // The listening socket is active immediately after server construction; a successful connect
    // simply confirms the OS accepted it. We retry briefly to absorb transient startup latency.
    ClientConnection cnx(server.port(), timeout);
  } catch (const std::exception& ex) {
    log_noexcept::error("ClientConnection constructor failed: {}", ex.what());
  }
}

#ifdef AERONET_WINDOWS
constexpr int kMaxCachedConnections = 0;
#else
constexpr int kMaxCachedConnections = 1;
#endif

SingleHttpServer CreateServer() noexcept {
  SingleHttpServer server;
  try {
    server = SingleHttpServer(HttpServerConfig{}
                                  .withPollInterval(std::chrono::milliseconds{1})
                                  .withMaxCachedConnections(kMaxCachedConnections),
                              RouterConfig{});
    server.start();
    WaitReady(server, std::chrono::milliseconds{500});
  } catch (const std::exception& ex) {
    log_noexcept::error("TestServer failed to start: {}", ex.what());
  }
  return server;
}

SingleHttpServer CreateServer(HttpServerConfig cfg, RouterConfig routerCfg, std::chrono::milliseconds poll) noexcept {
  SingleHttpServer server;
  try {
    server = SingleHttpServer(std::move(cfg.withPollInterval(poll).withMaxCachedConnections(kMaxCachedConnections)),
                              std::move(routerCfg));
    server.start();
    WaitReady(server, std::chrono::milliseconds{500});
  } catch (const std::exception& ex) {
    log_noexcept::error("TestServer failed to start: {}", ex.what());
  }
  return server;
}

}  // namespace

TestServer::TestServer() noexcept : server(CreateServer()) {}

TestServer::TestServer(HttpServerConfig cfg, RouterConfig routerCfg, std::chrono::milliseconds poll) noexcept
    : server(CreateServer(std::move(cfg), std::move(routerCfg), poll)) {}

uint16_t TestServer::port() const noexcept {
  try {
    return server.port();
  } catch (const std::exception& ex) {
    log_noexcept::error("TestServer exception in port(): {}", ex.what());
  }
  return 0;
}

void TestServer::resetConfig() {
  postConfigUpdate([](HttpServerConfig& cfg) {
    const auto port = cfg.port;
    cfg = HttpServerConfig{};
    cfg.withPort(port);
  });
}

void TestServer::postConfigUpdate(std::function<void(HttpServerConfig&)> updater) {
  auto completion = std::make_shared<std::promise<void>>();
  auto future = completion->get_future();

  server.postConfigUpdate([completion, updater = std::move(updater)](HttpServerConfig& config) mutable {
    try {
      updater(config);
    } catch (...) {
      completion->set_value();
      throw;
    }
    completion->set_value();
  });

  const auto waitTimeout = std::max(server.config().pollInterval * 10, std::chrono::milliseconds{200});
  if (future.wait_for(waitTimeout) == std::future_status::timeout) {
    log::warn("Config update did not complete within {} ms", waitTimeout.count());
    // Fallback with a hard deadline to avoid hanging the test suite on a stuck event loop.
    if (future.wait_for(std::chrono::seconds{10}) == std::future_status::timeout) {
      throw std::runtime_error("postConfigUpdate: server event loop appears stuck (10 s timeout)");
    }
  }
}

void TestServer::resetConfigAndPostUpdate(std::function<void(HttpServerConfig&)> updater) {
  postConfigUpdate([updater = std::move(updater)](HttpServerConfig& cfg) mutable {
    const auto port = cfg.port;
    cfg = HttpServerConfig{};  // Reset to default
    cfg.withPort(port);
    updater(cfg);
  });
}

void TestServer::postRouterUpdate(std::function<void(Router&)> updater) {
  auto completion = std::make_shared<std::promise<void>>();
  auto future = completion->get_future();

  server.postRouterUpdate([completion, updater = std::move(updater)](Router& router) mutable {
    try {
      updater(router);
    } catch (...) {
      completion->set_value();
      throw;
    }
    completion->set_value();
  });

  const auto waitTimeout = std::max(server.config().pollInterval * 10, std::chrono::milliseconds{200});
  if (future.wait_for(waitTimeout) == std::future_status::timeout) {
    log::warn("Router update did not complete within {} ms", waitTimeout.count());
    // Fallback with a hard deadline to avoid hanging the test suite on a stuck event loop.
    if (future.wait_for(std::chrono::seconds{10}) == std::future_status::timeout) {
      throw std::runtime_error("postRouterUpdate: server event loop appears stuck (10 s timeout)");
    }
  }
}

RouterUpdateProxy TestServer::resetRouterAndGet(std::function<void(Router&)> initializer) {
  resetRouter(std::move(initializer));
  return router();
}

void TestServer::resetRouter(std::function<void(Router&)> initializer) {
  postRouterUpdate([init = std::move(initializer)](Router& router) mutable {
    router.clear();
    if (init) {
      init(router);
    }
  });
}

void LogScopedConfigUpdateDestructorError() {
  log::error("Failed to restore HttpServerConfig field in ScopedConfigUpdate destructor");
}

bool WaitForServer(SingleHttpServer& server, bool running, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);  // NOLINT(misc-include-cleaner)
    if (server.isRunning() == running) {
      return true;
    }
  }
  return server.isRunning() == running;
}

}  // namespace aeronet::test
