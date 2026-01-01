#include "aeronet/test_server_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-server-config.hpp"
#include "aeronet/log.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router-update-proxy.hpp"
#include "aeronet/router.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/test_util.hpp"

namespace aeronet::test {

TestServer::TestServer(HttpServerConfig cfg, RouterConfig routerCfg, std::chrono::milliseconds pollPeriod)
    : server(std::move(cfg.withPollInterval(pollPeriod).withMaxCachedConnections(1)), std::move(routerCfg)) {
  server.start();
  waitReady(std::chrono::milliseconds{500});
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
    future.wait();
  }
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
    future.wait();
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

void TestServer::waitReady(std::chrono::milliseconds timeout) const {
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
  if (cnx.fd() == -1) {
    throw std::runtime_error("server readiness connect failed");
  }
}

bool WaitForServer(SingleHttpServer& server, bool running, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
    if (server.isRunning() == running) {
      return true;
    }
  }
  return server.isRunning() == running;
}

}  // namespace aeronet::test
