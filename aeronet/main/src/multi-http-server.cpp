#include "aeronet/multi-http-server.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/router.hpp"
#include "log.hpp"

namespace aeronet {

MultiHttpServer::MultiHttpServer(HttpServerConfig cfg, Router router, uint32_t threadCount)
    : _stopRequested(std::make_unique<std::atomic<bool>>(false)) {
  if (threadCount == 0) {
    throw std::invalid_argument("MultiHttpServer: threadCount must be >= 1");
  }
  // Prepare base config: if multiple threads, enforce reusePort.
  if (threadCount > 1 && !cfg.reusePort) {
    throw std::invalid_argument("MultiHttpServer: reusePort must be set for multi thread MultiHttpServer");
  }

  // Create the HttpServer (and ensure port resolution if given port is 0)
  _servers.reserve(static_cast<decltype(_servers)::size_type>(threadCount));

  // Construct first server. If port was ephemeral (0), HttpServer constructor resolves it synchronously.
  // We move the base config into the first server then copy back the resolved version (with concrete port).
  _servers.emplace_back(std::move(cfg), std::move(router));
}

MultiHttpServer::MultiHttpServer(HttpServerConfig cfg, Router router)
    : MultiHttpServer(
          HttpServerConfig(std::move(cfg)),  // make a copy for base storage
          std::move(router), []() {
            auto hc = std::thread::hardware_concurrency();
            if (hc == 0) {
              hc = 1;
              log::warn("Unable to detect the number of available processors for MultiHttpServer - defaults to {}", hc);
            }
            log::debug("MultiHttpServer auto-thread constructor detected hw_concurrency={}", hc);
            return static_cast<uint32_t>(hc);
          }()) {}

MultiHttpServer::MultiHttpServer(MultiHttpServer&& other) noexcept
    : _stopRequested(std::move(other._stopRequested)),
      _servers(std::move(other._servers)),
      _threads(std::move(other._threads)) {}

MultiHttpServer& MultiHttpServer::operator=(MultiHttpServer&& other) noexcept {
  if (this != &other) {
    // Ensure we are not leaking running threads; stop existing group first.
    stop();

    _stopRequested = std::move(other._stopRequested);
    _servers = std::move(other._servers);
    _threads = std::move(other._threads);
  }
  return *this;
}

MultiHttpServer::~MultiHttpServer() { stop(); }

void MultiHttpServer::postRouterUpdate(std::function<void(Router&)> updater) {
  if (empty()) {
    throw std::logic_error("Cannot post a router update on an empty MultiHttpServer");
  }

  auto sharedUpdater = std::make_shared<std::function<void(Router&)>>(std::move(updater));
  for (auto& server : _servers) {
    server.postRouterUpdate([sharedUpdater](Router& router) { (*sharedUpdater)(router); });
  }
}

RouterUpdateProxy MultiHttpServer::router() {
  if (empty()) {
    throw std::logic_error("Cannot access router proxy on an empty MultiHttpServer");
  }
  return {[this](std::function<void(Router&)> fn) { this->postRouterUpdate(std::move(fn)); },
          [this]() -> Router& { return this->_servers.front()._router; }};
}

std::string MultiHttpServer::AggregatedStats::json_str() const {
  std::string out;
  out.reserve(128UL * per.size());
  out.push_back('[');
  bool first = true;
  for (const auto& st : per) {
    if (first) {
      first = false;
    } else {
      out.push_back(',');
    }
    out.append(st.json_str());
  }
  out.push_back(']');
  return out;
}

void MultiHttpServer::canSetCallbacks() const {
  if (!_threads.empty()) {
    // Only disallow mutations while the server farm is actively running. After stop() (threads cleared)
    // handlers and callbacks may be adjusted prior to a restart.
    throw std::logic_error("Cannot mutate configuration while running (stop() first)");
  }
  if (_servers.empty()) {
    throw std::logic_error("Cannot set callbacks on an empty MultiHttpServer");
  }
}

void MultiHttpServer::setParserErrorCallback(HttpServer::ParserErrorCallback cb) {
  canSetCallbacks();
  _servers.front().setParserErrorCallback(std::move(cb));
}

void MultiHttpServer::setMetricsCallback(HttpServer::MetricsCallback cb) {
  canSetCallbacks();
  _servers.front().setMetricsCallback(std::move(cb));
}

void MultiHttpServer::setExpectationHandler(HttpServer::ExpectationHandler handler) {
  canSetCallbacks();
  _servers.front().setExpectationHandler(std::move(handler));
}

void MultiHttpServer::setMiddlewareMetricsCallback(HttpServer::MiddlewareMetricsCallback cb) {
  canSetCallbacks();
  _servers.front().setMiddlewareMetricsCallback(std::move(cb));
}

void MultiHttpServer::start() {
  if (!_threads.empty()) {
    throw std::logic_error("MultiHttpServer already started");
  }

  // Create the remaining servers.
  // firstServer reference is stable within the loop, because we have called reserved in the constructor.
  HttpServer& firstServer = _servers[0];
  while (_servers.size() < _servers.capacity()) {
    auto& server = _servers.emplace_back(firstServer.config(), firstServer._router);

    server.setParserErrorCallback(firstServer._parserErrCb);
    server.setMetricsCallback(firstServer._metricsCb);
    server.setExpectationHandler(firstServer._expectationHandler);
    server.setMiddlewareMetricsCallback(firstServer._middlewareMetricsCb);
  }

  _stopRequested->store(false, std::memory_order_relaxed);

  log::debug("MultiHttpServer starting with {} thread(s) on port :{}", _servers.size(), port());

  // Note: reserve is important here to guarantee pointer stability
  _threads.reserve(static_cast<decltype(_threads)::size_type>(_servers.size()));

  // Launch threads (each captures a stable pointer to its HttpServer element).
  for (std::size_t threadPos = 0; threadPos < _servers.size(); ++threadPos) {
    // srvPtr and stopRequested should remain constant through the whole run duration.
    // If the MultiHttpServer has been moved, the unique_ptr and vector containers ensure that these pointers stay
    // valid.
    HttpServer* srvPtr = &_servers[static_cast<vector<HttpServer>::size_type>(threadPos)];
    std::atomic<bool>* stopRequested = _stopRequested.get();
    _threads.emplace_back([stopRequested, srvPtr, threadPos]() {
      try {
        srvPtr->runUntil([stopRequested]() { return stopRequested->load(std::memory_order_relaxed); });
      } catch (const std::exception& ex) {
        log::error("Server thread {} terminated with exception: {}", threadPos, ex.what());
      } catch (...) {
        log::error("Server thread {} terminated with unknown exception", threadPos);
      }
    });
  }
  log::info("MultiHttpServer started with {} thread(s) on port :{}", _servers.size(), port());
}

void MultiHttpServer::stop() noexcept {
  if (_threads.empty() || _servers.empty()) {
    return;
  }
  _stopRequested->store(true, std::memory_order_relaxed);
  log::debug("MultiHttpServer stopping (instances={})", _servers.size());
  std::ranges::for_each(_servers, [](HttpServer& server) { server.stop(); });

  _threads.clear();
  _servers.erase(std::next(_servers.begin()), _servers.end());
  log::info("MultiHttpServer stopped");
}

void MultiHttpServer::beginDrain(std::chrono::milliseconds maxWait) noexcept {
  std::ranges::for_each(_servers, [maxWait](HttpServer& server) { server.beginDrain(maxWait); });
}

bool MultiHttpServer::isDraining() const {
  return std::ranges::any_of(_servers, [](const HttpServer& server) { return server.isDraining(); });
}

MultiHttpServer::AggregatedStats MultiHttpServer::stats() const {
  AggregatedStats agg;
  agg.per.reserve(_servers.size());
  for (auto& server : _servers) {
    auto st = server.stats();
    agg.total.totalBytesQueued += st.totalBytesQueued;
    agg.total.totalBytesWrittenImmediate += st.totalBytesWrittenImmediate;
    agg.total.totalBytesWrittenFlush += st.totalBytesWrittenFlush;
    agg.total.deferredWriteEvents += st.deferredWriteEvents;
    agg.total.flushCycles += st.flushCycles;
    agg.total.epollModFailures += st.epollModFailures;
    agg.total.maxConnectionOutboundBuffer =
        std::max(agg.total.maxConnectionOutboundBuffer, st.maxConnectionOutboundBuffer);
    agg.total.totalRequestsServed += st.totalRequestsServed;
#ifdef AERONET_ENABLE_OPENSSL
    agg.total.tlsHandshakesSucceeded += st.tlsHandshakesSucceeded;
    agg.total.tlsClientCertPresent += st.tlsClientCertPresent;
    agg.total.tlsAlpnStrictMismatches += st.tlsAlpnStrictMismatches;
    for (const auto& kv : st.tlsAlpnDistribution) {
      bool found = false;
      for (auto& existing : agg.total.tlsAlpnDistribution) {
        if (existing.first == kv.first) {
          existing.second += kv.second;
          found = true;
          break;
        }
      }
      if (!found) {
        agg.total.tlsAlpnDistribution.push_back(kv);
      }
    }
    for (const auto& kv : st.tlsVersionCounts) {
      bool found = false;
      for (auto& existing : agg.total.tlsVersionCounts) {
        if (existing.first == kv.first) {
          existing.second += kv.second;
          found = true;
          break;
        }
      }
      if (!found) {
        agg.total.tlsVersionCounts.push_back(kv);
      }
    }
    for (const auto& [newKey, newVal] : st.tlsCipherCounts) {
      bool found = false;
      for (auto& existing : agg.total.tlsCipherCounts) {
        if (existing.first == newKey) {
          existing.second += newVal;
          found = true;
          break;
        }
      }
      if (!found) {
        agg.total.tlsCipherCounts.emplace_back(newKey, newVal);
      }
    }
    agg.total.tlsHandshakeDurationCount += st.tlsHandshakeDurationCount;
    agg.total.tlsHandshakeDurationTotalNs += st.tlsHandshakeDurationTotalNs;
    agg.total.tlsHandshakeDurationMaxNs = std::max(agg.total.tlsHandshakeDurationMaxNs, st.tlsHandshakeDurationMaxNs);
#endif
    agg.per.push_back(std::move(st));
  }
  log::trace("Aggregated stats across {} server instance(s)", agg.per.size());
  return agg;
}

void MultiHttpServer::postConfigUpdate(const std::function<void(HttpServerConfig&)>& updater) {
  for (auto& server : _servers) {
    server.postConfigUpdate(updater);
  }
}

}  // namespace aeronet
