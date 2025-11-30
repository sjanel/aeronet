#include "aeronet/multi-http-server.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/errno-throw.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/log.hpp"
#include "aeronet/router-update-proxy.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-lifecycle-tracker.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

struct MultiHttpServer::HandleCompletion {
  void notify() {
    {
      std::scoped_lock lock(mutex);
      if (completed) {
        return;
      }
      completed = true;
    }
    cv.notify_all();
  }

  void wait() {
    std::unique_lock lock(mutex);
    cv.wait(lock, [this] { return completed; });
  }

  std::mutex mutex;
  std::condition_variable cv;
  bool completed{false};
};

MultiHttpServer::AsyncHandle::AsyncHandle(vector<HttpServer::AsyncHandle> serverHandles,
                                          std::shared_ptr<std::atomic<bool>> stopRequested,
                                          std::shared_ptr<std::function<void()>> onStop,
                                          std::shared_ptr<HandleCompletion> completion,
                                          std::shared_ptr<ServerLifecycleTracker> lifecycleTracker,
                                          std::shared_ptr<void> stopTokenBinding)
    : _serverHandles(std::move(serverHandles)),
      _stopRequested(std::move(stopRequested)),
      _onStop(std::move(onStop)),
      _completion(std::move(completion)),
      _lifecycleTracker(std::move(lifecycleTracker)),
      _stopTokenBinding(std::move(stopTokenBinding)) {}

MultiHttpServer::AsyncHandle::AsyncHandle(AsyncHandle&& other) noexcept
    : _serverHandles(std::move(other._serverHandles)),
      _stopRequested(std::move(other._stopRequested)),
      _onStop(std::move(other._onStop)),
      _completion(std::move(other._completion)),
      _lifecycleTracker(std::move(other._lifecycleTracker)),
      _stopTokenBinding(std::move(other._stopTokenBinding)),
      _stopCalled(other._stopCalled.exchange(true, std::memory_order_relaxed)) {}

MultiHttpServer::AsyncHandle& MultiHttpServer::AsyncHandle::operator=(AsyncHandle&& other) noexcept {
  if (this != &other) {
    stop();

    _serverHandles = std::move(other._serverHandles);
    _stopRequested = std::move(other._stopRequested);
    _onStop = std::move(other._onStop);
    _completion = std::move(other._completion);
    _lifecycleTracker = std::move(other._lifecycleTracker);
    _stopTokenBinding = std::move(other._stopTokenBinding);
    _stopCalled.store(other._stopCalled.exchange(true, std::memory_order_relaxed), std::memory_order_relaxed);
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
    if (_lifecycleTracker) {
      _lifecycleTracker->notifyStopRequested();
    }
  }

  if (_onStop && *_onStop) {
    (*_onStop)();
  }

  for (auto& handle : _serverHandles) {
    handle.stop();
  }

  // Release the shared stop callback so any weak_ptr held by the MultiHttpServer
  // instance can expire when the caller has stopped the AsyncHandle. Clear the
  // local thread vector to reflect there are no active background threads.
  _onStop.reset();
  _serverHandles.clear();
  notifyCompletion();
}

void MultiHttpServer::AsyncHandle::rethrowIfError() {
  for (auto& handle : _serverHandles) {
    handle.rethrowIfError();
  }
}

void MultiHttpServer::AsyncHandle::notifyCompletion() noexcept {
  if (_completion) {
    _completion->notify();
    _completion.reset();
  }
}

bool MultiHttpServer::AsyncHandle::started() const noexcept {
  return std::ranges::any_of(_serverHandles, [](const auto& handle) { return handle.started(); });
}

MultiHttpServer::MultiHttpServer(HttpServerConfig cfg, Router router, uint32_t threadCount)
    : _stopRequested(std::make_shared<std::atomic<bool>>(false)) {
  if (threadCount == 0) {
    threadCount = std::thread::hardware_concurrency();
    if (threadCount == 0) {
      threadCount = 1;
      log::warn("Unable to detect the number of available processors for MultiHttpServer - defaults to {}",
                threadCount);
    }
    log::debug("MultiHttpServer auto-thread constructor detected hw_concurrency={}", threadCount);
  }

  _servers.reserve(static_cast<decltype(_servers)::size_type>(threadCount));

  if (threadCount > 1U && !cfg.reusePort) {
    if (cfg.port != 0) {
      // User wants to ensure that the given port will be exclusively used by this MultiHttpServer.
      // We need reusePort to be enabled for multiple threads to bind to the same port, but we can
      // verify that no other process is using the port by attempting to bind once here.
      // If the port is already in use, this will throw.
      if (!Socket{Socket::Type::StreamNonBlock}.tryBind(cfg.reusePort, cfg.tcpNoDelay, cfg.port)) {
        throw_errno("bind failed on this port - already in use");
      }
      // immediately close the socket again to free the port for the actual servers below.
    }

    cfg.reusePort = true;  // enforce reusePort for multi-threaded servers
    log::debug("MultiHttpServer: Enabling reusePort for multi-threaded server");
  }

  auto& firstServer = _servers.emplace_back(std::move(cfg), std::move(router));

  firstServer._lifecycleTracker = _lifecycleTracker;
  firstServer._isInMultiHttpServer = true;
}

MultiHttpServer::MultiHttpServer(const MultiHttpServer& other)
    : _stopRequested(std::make_shared<std::atomic<bool>>(false)),
      _lifecycleTracker(std::make_shared<ServerLifecycleTracker>()),
      _serversAlive(std::make_shared<std::atomic<bool>>(true)) {
  if (other.isRunning()) {
    throw std::logic_error("Cannot copy-construct a running MultiHttpServer");
  }

  _servers.reserve(other._servers.capacity());
  for (const auto& server : other._servers) {
    auto& cloned = _servers.emplace_back(server);
    cloned._lifecycleTracker = _lifecycleTracker;
  }
}

MultiHttpServer::MultiHttpServer(MultiHttpServer&& other) noexcept
    : _stopRequested(std::move(other._stopRequested)),
      _lifecycleTracker(std::move(other._lifecycleTracker)),
      _servers(std::move(other._servers)),
      _internalHandle(std::move(other._internalHandle)),
      _lastHandleStopFn(std::move(other._lastHandleStopFn)),
      _lastHandleCompletion(std::move(other._lastHandleCompletion)),
      _serversAlive(std::move(other._serversAlive)) {
  for (auto& server : _servers) {
    server._lifecycleTracker = _lifecycleTracker;
  }
}

MultiHttpServer& MultiHttpServer::operator=(MultiHttpServer&& other) noexcept {
  if (this != &other) {
    // Ensure we are not leaking running threads; stop existing group first.
    stop();

    _stopRequested = std::move(other._stopRequested);
    _lifecycleTracker = std::move(other._lifecycleTracker);
    _servers = std::move(other._servers);
    _internalHandle = std::move(other._internalHandle);
    _lastHandleStopFn = std::move(other._lastHandleStopFn);
    _lastHandleCompletion = std::move(other._lastHandleCompletion);
    _serversAlive = std::move(other._serversAlive);

    for (auto& server : _servers) {
      server._lifecycleTracker = _lifecycleTracker;
    }
  }
  return *this;
}

MultiHttpServer& MultiHttpServer::operator=(const MultiHttpServer& other) {
  if (this != &other) {
    if (other.isRunning()) {
      throw std::logic_error("Cannot copy-assign a running MultiHttpServer");
    }

    stop();

    MultiHttpServer copy(other);
    using std::swap;
    swap(*this, copy);
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
  _lifecycleTracker->notifyStopRequested();

  log::debug("MultiHttpServer stopping (instances={})", _servers.size());
  std::ranges::for_each(_servers, [](HttpServer& server) { server.stop(); });

  // Stop internal handle if start() was used (non-blocking API)
  if (_internalHandle) {
    _internalHandle->stop();
    _internalHandle.reset();
  }

  if (auto completion = _lastHandleCompletion.lock()) {
    completion->wait();
  }
  log::info("MultiHttpServer stopped");
}

void MultiHttpServer::start() { _internalHandle.emplace(startDetached()); }

MultiHttpServer::AsyncHandle MultiHttpServer::startDetached() {
  return startDetachedInternal([] { return false; }, {});
}

MultiHttpServer::AsyncHandle MultiHttpServer::startDetachedAndStopWhen(std::function<bool()> predicate) {
  return startDetachedInternal(std::move(predicate), {});
}

MultiHttpServer::AsyncHandle MultiHttpServer::startDetachedWithStopToken(const std::stop_token& token) {
  if (!token.stop_possible()) {
    return startDetachedInternal([] { return false; }, {});
  }
  return startDetachedInternal([tokenCopy = token]() { return tokenCopy.stop_requested(); }, token);
}

void MultiHttpServer::beginDrain(std::chrono::milliseconds maxWait) noexcept {
  std::ranges::for_each(_servers, [maxWait](HttpServer& server) { server.beginDrain(maxWait); });
}

bool MultiHttpServer::isDraining() const {
  return std::ranges::any_of(_servers, [](const HttpServer& server) { return server.isDraining(); });
}

void MultiHttpServer::postConfigUpdate(const std::function<void(HttpServerConfig&)>& updater) {
  if (empty()) {
    throw std::logic_error("Cannot post a config update on an empty MultiHttpServer");
  }
  std::ranges::for_each(_servers, [&updater](HttpServer& server) { server.postConfigUpdate(updater); });
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
  if (isRunning()) {
    // Only disallow mutations while the server farm is actively running
    throw std::logic_error("Cannot mutate configuration while running (stop() first)");
  }
  if (_servers.empty()) {
    throw std::logic_error("Cannot set callbacks on an empty MultiHttpServer");
  }
}

void MultiHttpServer::ensureNextServersBuilt() {
  if (_servers.empty()) {
    throw std::logic_error("Cannot rebuild servers on an empty MultiHttpServer");
  }

  auto& firstServer = _servers.front();

  firstServer.applyPendingUpdates();

  const auto targetCount = _servers.capacity();

  _servers.resize(1UL);

  while (_servers.size() < targetCount) {
    auto& nextServer = _servers.emplace_back(firstServer);
    nextServer._lifecycleTracker = _lifecycleTracker;
  }
}

vector<HttpServer*> MultiHttpServer::collectServerPointers() {
  vector<HttpServer*> serverPtrs;
  serverPtrs.reserve(_servers.size());
  std::ranges::transform(_servers, std::back_inserter(serverPtrs), [](HttpServer& server) { return &server; });
  return serverPtrs;
}

void MultiHttpServer::runBlocking(std::function<bool()> predicate, std::string_view modeLabel) {
  if (_servers.empty()) {
    throw std::logic_error("Cannot run an empty MultiHttpServer");
  }

  if (isRunning()) {
    throw std::logic_error("MultiHttpServer already started");
  }

  // Use a local AsyncHandle to manage the servers.
  // We do NOT store it in _internalHandle to avoid race conditions with stop().
  // stop() will signal _stopRequested and wait for us via _lastHandleStopFn.
  AsyncHandle handle = startDetachedInternal(std::move(predicate), {});

  const bool started = _lifecycleTracker->waitUntilAnyRunning(*_stopRequested);
  if (!_stopRequested->load(std::memory_order_relaxed) && started) {
    _lifecycleTracker->waitUntilAllStopped(*_stopRequested);
  }

  log::info("MultiHttpServer {}{}stopped", modeLabel, modeLabel.empty() ? "" : " ");

  // handle goes out of scope here, stopping and joining all threads.
}

MultiHttpServer::AsyncHandle MultiHttpServer::startDetachedInternal(std::function<bool()> extraStopCondition,
                                                                    const std::stop_token& externalStopToken) {
  if (_servers.empty()) {
    throw std::logic_error("Cannot start an empty MultiHttpServer");
  }
  if (isRunning()) {
    throw std::logic_error("MultiHttpServer already started");
  }

  _lifecycleTracker->clear();

  // Create the remaining servers.
  ensureNextServersBuilt();

  _stopRequested->store(false, std::memory_order_relaxed);

  log::debug("MultiHttpServer starting with {} thread(s) on port :{}", _servers.size(), port());

  vector<HttpServer::AsyncHandle> serverHandles;
  serverHandles.reserve(_servers.size());

  if (!extraStopCondition) {
    extraStopCondition = [] { return false; };
  }

  auto serverPtrs = collectServerPointers();
  auto lifecycleTracker = _lifecycleTracker;

  auto serversAlive = _serversAlive;
  auto stopCallback =
      std::make_shared<std::function<void()>>([serverPtrs = std::move(serverPtrs), serversAlive]() noexcept {
        if (serversAlive && serversAlive->load(std::memory_order_acquire)) {
          std::ranges::for_each(serverPtrs, [](HttpServer* srv) { srv->stop(); });
        }
      });

  _lastHandleStopFn = stopCallback;
  auto handleCompletion = std::make_shared<HandleCompletion>();
  _lastHandleCompletion = handleCompletion;

  std::shared_ptr<void> externalStopBinding;
  if (externalStopToken.stop_possible()) {
    auto stopAction = [stopRequested = _stopRequested, stopCallback, lifecycleTracker]() {
      const bool alreadySet = stopRequested->exchange(true, std::memory_order_acq_rel);
      if (!alreadySet && stopCallback && *stopCallback) {
        (*stopCallback)();
      }
      if (lifecycleTracker) {
        lifecycleTracker->notifyStopRequested();
      }
    };
    externalStopBinding =
        std::make_shared<std::stop_callback<decltype(stopAction)>>(externalStopToken, std::move(stopAction));
  }

  // Launch threads (each captures a stable pointer to its HttpServer element).
  for (auto& server : _servers) {
    std::atomic<bool>* stopRequested = _stopRequested.get();
    auto threadExtraStop = extraStopCondition;
    serverHandles.push_back(
        server.startDetachedAndStopWhen([stopRequested, threadExtraStop, stopCallback, lifecycleTracker]() {
          if (stopRequested->load(std::memory_order_relaxed)) {
            return true;
          }
          if (threadExtraStop()) {
            const bool alreadySet = stopRequested->exchange(true, std::memory_order_acq_rel);
            if (!alreadySet && stopCallback && *stopCallback) {
              (*stopCallback)();
              if (lifecycleTracker) {
                lifecycleTracker->notifyStopRequested();
              }
            }
            return true;
          }
          return false;
        }));
  }
  log::info("MultiHttpServer started with {} thread(s) on port :{}", _servers.size(), port());

  // Move threads into the handle - this clears _threads so isRunning() will return false
  // but the handle owns the threads now.
  // Share the _stopRequested pointer so both MultiHttpServer and AsyncHandle can control stopping.
  return {std::move(serverHandles),    _stopRequested,    std::move(stopCallback),
          std::move(handleCompletion), _lifecycleTracker, std::move(externalStopBinding)};
}

MultiHttpServer::AggregatedStats MultiHttpServer::stats() const {
  AggregatedStats agg;
  agg.per.reserve(_servers.size());
  for (const auto& server : _servers) {
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
      auto it = std::ranges::find_if(agg.total.tlsAlpnDistribution,
                                     [&kv](const auto& existing) { return existing.first == kv.first; });
      if (it != agg.total.tlsAlpnDistribution.end()) {
        it->second += kv.second;
      } else {
        agg.total.tlsAlpnDistribution.push_back(kv);
      }
    }
    for (const auto& kv : st.tlsVersionCounts) {
      auto it = std::ranges::find_if(agg.total.tlsVersionCounts,
                                     [&kv](const auto& existing) { return existing.first == kv.first; });
      if (it != agg.total.tlsVersionCounts.end()) {
        it->second += kv.second;
      } else {
        agg.total.tlsVersionCounts.push_back(kv);
      }
    }
    for (const auto& [newKey, newVal] : st.tlsCipherCounts) {
      auto it = std::ranges::find_if(agg.total.tlsCipherCounts,
                                     [&newKey](const auto& existing) { return existing.first == newKey; });
      if (it != agg.total.tlsCipherCounts.end()) {
        it->second += newVal;
      } else {
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
