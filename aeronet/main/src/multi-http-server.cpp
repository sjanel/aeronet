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
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/log.hpp"
#include "aeronet/router-update-proxy.hpp"
#include "aeronet/router.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

MultiHttpServer::AsyncHandle::AsyncHandle(vector<std::jthread> threads, std::shared_ptr<std::exception_ptr> error,
                                          std::shared_ptr<std::atomic<bool>> stopRequested,
                                          std::shared_ptr<std::function<void()>> onStop)
    : _threads(std::move(threads)),
      _error(std::move(error)),
      _stopRequested(std::move(stopRequested)),
      _onStop(std::move(onStop)) {}

MultiHttpServer::AsyncHandle::AsyncHandle(AsyncHandle&& other) noexcept
    : _threads(std::move(other._threads)),
      _error(std::move(other._error)),
      _stopRequested(std::move(other._stopRequested)),
      _onStop(std::move(other._onStop)),
      _stopCalled(other._stopCalled.load(std::memory_order_relaxed)) {
  other._stopCalled.store(true, std::memory_order_relaxed);
}

MultiHttpServer::AsyncHandle& MultiHttpServer::AsyncHandle::operator=(AsyncHandle&& other) noexcept {
  if (this != &other) {
    stop();

    _threads = std::move(other._threads);
    _error = std::move(other._error);
    _stopRequested = std::move(other._stopRequested);
    _onStop = std::move(other._onStop);
    _stopCalled.store(other._stopCalled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    other._stopCalled.store(true, std::memory_order_relaxed);
  }
  return *this;
}

MultiHttpServer::AsyncHandle::~AsyncHandle() { stop(); }

void MultiHttpServer::AsyncHandle::stop() noexcept {
  if (_stopCalled.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  if (_stopRequested) {
    _stopRequested->store(true, std::memory_order_relaxed);
  }

  if (_onStop && *_onStop) {
    (*_onStop)();
  }

  for (auto& thread : _threads) {
    if (thread.joinable()) {
      thread.request_stop();
      thread.join();
    }
  }
}

void MultiHttpServer::AsyncHandle::rethrowIfError() {
  if (_error && *_error) {
    std::rethrow_exception(*_error);
  }
}

bool MultiHttpServer::AsyncHandle::started() const noexcept {
  return std::ranges::any_of(_threads, [](const std::jthread& thread) { return thread.joinable(); });
}

MultiHttpServer::MultiHttpServer(HttpServerConfig cfg, Router router, uint32_t threadCount)
    : _stopRequested(std::make_shared<std::atomic<bool>>(false)) {
  if (threadCount == 0) {
    throw std::invalid_argument("MultiHttpServer: threadCount must be >= 1");
  }
  // Prepare base config: if multiple threads, enforce reusePort.
  if (threadCount > 1 && !cfg.reusePort) {
    throw std::invalid_argument("MultiHttpServer: reusePort must be set for multi thread MultiHttpServer");
  }

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
      _threads(std::move(other._threads)),
      _internalHandle(std::move(other._internalHandle)),
      _lastHandleStopFn(std::move(other._lastHandleStopFn)),
      _serversAlive(std::move(other._serversAlive)) {}

MultiHttpServer& MultiHttpServer::operator=(MultiHttpServer&& other) noexcept {
  if (this != &other) {
    // Ensure we are not leaking running threads; stop existing group first.
    stop();

    _stopRequested = std::move(other._stopRequested);
    _servers = std::move(other._servers);
    _threads = std::move(other._threads);
    _internalHandle = std::move(other._internalHandle);
    _lastHandleStopFn = std::move(other._lastHandleStopFn);
    _serversAlive = std::move(other._serversAlive);
  }
  return *this;
}

MultiHttpServer::~MultiHttpServer() {
  stop();

  if (auto cb = _lastHandleStopFn.lock()) {
    *cb = []() {};
  }

  if (_serversAlive) {
    _serversAlive->store(false, std::memory_order_release);
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
  for (const auto& st : per) {
    if (out.size() > 1UL) {
      out.push_back(',');
    }
    out.append(st.json_str());
  }
  out.push_back(']');
  return out;
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

void MultiHttpServer::run() { runBlocking({}, ""); }

void MultiHttpServer::runUntil(const std::function<bool()>& predicate) {
  runBlocking(std::function<bool()>{predicate}, "runUntil");
}

void MultiHttpServer::stop() noexcept {
  if (_servers.empty()) {
    return;
  }
  _stopRequested->store(true, std::memory_order_relaxed);
  log::debug("MultiHttpServer stopping (instances={})", _servers.size());
  std::ranges::for_each(_servers, [](HttpServer& server) { server.stop(); });

  // Stop internal handle if start() was used (non-blocking API)
  if (_internalHandle.has_value()) {
    _internalHandle->stop();
    _internalHandle.reset();
  }

  // Note: Thread joining is now handled by AsyncHandle destructor if startDetached() was called.
  // If run() was called, threads are already joined by the time we get here.

  // Only after all threads have joined, clean up servers
  _servers.erase(std::next(_servers.begin()), _servers.end());
  log::info("MultiHttpServer stopped");
}

void MultiHttpServer::start() { _internalHandle.emplace(startDetached()); }

MultiHttpServer::AsyncHandle MultiHttpServer::startDetached() {
  return startDetachedInternal([] { return false; }, false);
}

MultiHttpServer::AsyncHandle MultiHttpServer::startDetachedAndStopWhen(std::function<bool()> predicate) {
  return startDetachedInternal(std::move(predicate), true);
}

MultiHttpServer::AsyncHandle MultiHttpServer::startDetachedWithStopToken(std::stop_token token) {
  return startDetachedInternal([token = std::move(token)]() mutable { return token.stop_requested(); }, true);
}

void MultiHttpServer::beginDrain(std::chrono::milliseconds maxWait) noexcept {
  std::ranges::for_each(_servers, [maxWait](HttpServer& server) { server.beginDrain(maxWait); });
}

bool MultiHttpServer::isDraining() const {
  return std::ranges::any_of(_servers, [](const HttpServer& server) { return server.isDraining(); });
}

void MultiHttpServer::postConfigUpdate(const std::function<void(HttpServerConfig&)>& updater) {
  for (auto& server : _servers) {
    server.postConfigUpdate(updater);
  }
}

void MultiHttpServer::postRouterUpdate(std::function<void(Router&)> updater) {
  if (empty()) {
    throw std::logic_error("Cannot post a router update on an empty MultiHttpServer");
  }

  auto sharedUpdater = std::make_shared<std::function<void(Router&)>>(std::move(updater));
  for (auto& server : _servers) {
    server.postRouterUpdate([sharedUpdater](Router& router) { (*sharedUpdater)(router); });
  }
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

void MultiHttpServer::rebuildServers() {
  if (_servers.empty()) {
    return;
  }

  _servers.erase(std::next(_servers.begin()), _servers.end());

  HttpServer& firstServer = _servers.front();
  firstServer._isInMultiHttpServer = true;
  while (_servers.size() < _servers.capacity()) {
    auto& server = _servers.emplace_back(firstServer.config(), firstServer._router);
    server.setParserErrorCallback(firstServer._parserErrCb);
    server.setMetricsCallback(firstServer._metricsCb);
    server.setExpectationHandler(firstServer._expectationHandler);
    server.setMiddlewareMetricsCallback(firstServer._middlewareMetricsCb);
    server._isInMultiHttpServer = true;
  }
}

vector<HttpServer*> MultiHttpServer::collectServerPointers() {
  vector<HttpServer*> serverPtrs;
  serverPtrs.reserve(_servers.size());
  for (auto& server : _servers) {
    serverPtrs.push_back(&server);
  }
  return serverPtrs;
}

void MultiHttpServer::runBlocking(std::function<bool()> predicate, std::string_view modeLabel) {
  if (_servers.empty()) {
    return;
  }

  if (!_threads.empty()) {
    throw std::logic_error("MultiHttpServer already started");
  }

  rebuildServers();

  _stopRequested->store(false, std::memory_order_relaxed);

  log::debug("MultiHttpServer {}{}starting with {} thread(s) on port :{}", modeLabel, modeLabel.empty() ? "" : " ",
             _servers.size(), port());

  _threads.reserve(static_cast<decltype(_threads)::size_type>(_servers.size()));

  if (!predicate) {
    for (std::size_t threadPos = 0; threadPos < _servers.size(); ++threadPos) {
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
  } else {
    auto sharedPredicate = std::move(predicate);
    for (std::size_t threadPos = 0; threadPos < _servers.size(); ++threadPos) {
      HttpServer* srvPtr = &_servers[static_cast<vector<HttpServer>::size_type>(threadPos)];
      std::atomic<bool>* stopRequested = _stopRequested.get();
      auto threadPredicate = sharedPredicate;
      _threads.emplace_back([stopRequested, srvPtr, threadPos, threadPredicate]() {
        try {
          srvPtr->runUntil([stopRequested, threadPredicate]() {
            if (stopRequested->load(std::memory_order_relaxed)) {
              return true;
            }
            if (threadPredicate && threadPredicate()) {
              stopRequested->store(true, std::memory_order_relaxed);
              return true;
            }
            return false;
          });
        } catch (const std::exception& ex) {
          log::error("Server thread {} terminated with exception: {}", threadPos, ex.what());
        } catch (...) {
          log::error("Server thread {} terminated with unknown exception", threadPos);
        }
      });
    }
  }

  log::info("MultiHttpServer {}{}started with {} thread(s) on port :{}", modeLabel, modeLabel.empty() ? "" : " ",
            _servers.size(), port());

  _threads.clear();

  _servers.erase(std::next(_servers.begin()), _servers.end());
  log::info("MultiHttpServer {}{}stopped", modeLabel, modeLabel.empty() ? "" : " ");
}

MultiHttpServer::AsyncHandle MultiHttpServer::startDetachedInternal(std::function<bool()> extraStopCondition,
                                                                    bool monitorPredicateAsync) {
  if (isRunning()) {
    throw std::logic_error("MultiHttpServer already started");
  }

  // Create the remaining servers.
  // firstServer reference is stable within the loop, because we have called reserved in the constructor.
  rebuildServers();

  _stopRequested->store(false, std::memory_order_relaxed);

  log::debug("MultiHttpServer starting with {} thread(s) on port :{}", _servers.size(), port());

  // Note: reserve is important here to guarantee pointer stability
  const auto expectedThreadCount = static_cast<decltype(_threads)::size_type>(_servers.size()) +
                                   static_cast<decltype(_threads)::size_type>(monitorPredicateAsync ? 1U : 0U);
  _threads.reserve(expectedThreadCount);

  // Shared exception pointer to capture the first error from any thread
  auto errorPtr = std::make_shared<std::exception_ptr>();
  auto errorCaptured = std::make_shared<std::atomic<bool>>(false);

  if (!extraStopCondition) {
    extraStopCondition = [] { return false; };
  }

  auto serverPtrs = collectServerPointers();

  auto serversAlive = _serversAlive;
  auto stopCallback =
      std::make_shared<std::function<void()>>([serverPtrs = std::move(serverPtrs), serversAlive]() noexcept {
        if (!serversAlive || !serversAlive->load(std::memory_order_acquire)) {
          return;
        }
        for (HttpServer* srv : serverPtrs) {
          srv->stop();
        }
      });

  _lastHandleStopFn = stopCallback;

  if (monitorPredicateAsync) {
    auto monitorExtraStop = extraStopCondition;
    auto monitorStopRequested = _stopRequested;
    _threads.emplace_back([monitorStopRequested, monitorExtraStop, stopCallback, errorPtr, errorCaptured]() {
      try {
        while (!monitorStopRequested->load(std::memory_order_relaxed)) {
          if (monitorExtraStop()) {
            const bool alreadySet = monitorStopRequested->exchange(true, std::memory_order_acq_rel);
            if (!alreadySet && stopCallback && *stopCallback) {
              (*stopCallback)();
            }
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      } catch (...) {
        bool expected = false;
        if (errorCaptured->compare_exchange_strong(expected, true)) {
          *errorPtr = std::current_exception();
          try {
            std::rethrow_exception(std::current_exception());
          } catch (const std::exception& ex) {
            log::error("Predicate monitor terminated with exception: {}", ex.what());
          } catch (...) {
            log::error("Predicate monitor terminated with unknown exception");
          }
        }
      }
    });
  }

  // Launch threads (each captures a stable pointer to its HttpServer element).
  for (std::size_t threadPos = 0; threadPos < _servers.size(); ++threadPos) {
    // srvPtr and stopRequested should remain constant through the whole run duration.
    // If the MultiHttpServer has been moved, the unique_ptr and vector containers ensure that these pointers stay
    // valid.
    HttpServer* srvPtr = &_servers[static_cast<vector<HttpServer>::size_type>(threadPos)];
    std::atomic<bool>* stopRequested = _stopRequested.get();
    auto threadExtraStop = extraStopCondition;
    _threads.emplace_back([stopRequested, srvPtr, threadPos, errorPtr, errorCaptured, threadExtraStop, stopCallback]() {
      try {
        srvPtr->runUntil([stopRequested, threadExtraStop, stopCallback]() {
          if (stopRequested->load(std::memory_order_relaxed)) {
            return true;
          }
          if (threadExtraStop()) {
            const bool alreadySet = stopRequested->exchange(true, std::memory_order_acq_rel);
            if (!alreadySet && stopCallback && *stopCallback) {
              (*stopCallback)();
            }
            return true;
          }
          return false;
        });
      } catch (...) {
        // Capture first exception only
        bool expected = false;
        if (errorCaptured->compare_exchange_strong(expected, true)) {
          *errorPtr = std::current_exception();
          try {
            std::rethrow_exception(std::current_exception());
          } catch (const std::exception& ex) {
            log::error("Server thread {} terminated with exception: {}", threadPos, ex.what());
          } catch (...) {
            log::error("Server thread {} terminated with unknown exception", threadPos);
          }
        }
      }
    });
  }
  log::info("MultiHttpServer started with {} thread(s) on port :{}", _servers.size(), port());

  // Move threads into the handle - this clears _threads so isRunning() will return false
  // but the handle owns the threads now.
  // Share the _stopRequested pointer so both MultiHttpServer and AsyncHandle can control stopping.
  return {std::move(_threads), errorPtr, _stopRequested, std::move(stopCallback)};
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

}  // namespace aeronet
