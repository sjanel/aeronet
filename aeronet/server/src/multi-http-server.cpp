#include "aeronet/multi-http-server.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/builtin-probes-config.hpp"
#include "aeronet/errno-throw.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/lifecycle.hpp"
#include "aeronet/log-noexcept.hpp"
#include "aeronet/log.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/router-update-proxy.hpp"
#include "aeronet/router.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/server-lifecycle-tracker.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_GLAZE
#include <filesystem>
#include <fstream>
#include <ios>

#include "aeronet/aeronet-config.hpp"
#include "aeronet/config-loader.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-ticket-key-store.hpp"
#endif

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

// Aggregated, read-only snapshot of the worker event loops consulted by the dedicated probe listener. The vector of
// Lifecycle pointers is repopulated on every start() (workers are rebuilt), while the shared_ptr keeps the object
// alive for as long as the probe route handlers (which capture it) exist.
struct MultiHttpServer::ProbeState {
  // Readiness: the pod can serve traffic if any worker is accepting normal traffic (all draining => not ready).
  [[nodiscard]] bool ready() const noexcept {
    return std::ranges::any_of(workers, [](const internal::Lifecycle* lc) { return lc->ready(); });
  }

  // Startup: the pod has started once any worker has entered its event loop.
  [[nodiscard]] bool started() const noexcept {
    return std::ranges::any_of(workers, [](const internal::Lifecycle* lc) { return lc->started(); });
  }

  // Liveness (heartbeat): the pod is live unless EVERY worker loop has stopped making progress beyond the threshold.
  // Each worker publishes a heartbeat at the top of every event loop iteration (internal::Lifecycle::loopHeartbeat);
  // a loop stuck inside a request handler (or otherwise not polling) stops advancing it. Live if at least one worker
  // is idle or still progressing, so a busy-but-progressing worker (or a free sibling) keeps the pod alive while a
  // full deadlock across all workers trips it.
  [[nodiscard]] bool live() const noexcept {
    // The probe listener is only built alongside a non-empty worker pool (see buildProbeServerIfEnabled).
    assert(!workers.empty());
    const std::int64_t nowNs = internal::SteadyNowNs();
    return std::ranges::any_of(
        workers, [&](const internal::Lifecycle* lc) { return lc->loopHealthy(nowNs, livenessThresholdNs); });
  }

  vector<const internal::Lifecycle*> workers;
  std::int64_t livenessThresholdNs{0};
};

MultiHttpServer::AsyncHandle::AsyncHandle(vector<SingleHttpServer::AsyncHandle> serverHandles,
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
  if (this != &other) [[likely]] {
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

  std::ranges::for_each(_serverHandles, [](auto& handle) { handle.stop(); });

  // Collect any stored errors before clearing the handles so rethrowIfError()
  // can still report them after stop() returns.
  for (auto& handle : _serverHandles) {
    try {
      handle.rethrowIfError();
    } catch (...) {
      if (!_storedError) {
        _storedError = std::current_exception();
      }
    }
  }

  // Release the shared stop callback so any weak_ptr held by the MultiHttpServer
  // instance can expire when the caller has stopped the AsyncHandle. Clear the
  // local thread vector to reflect there are no active background threads.
  _onStop.reset();
  _serverHandles.clear();
  notifyCompletion();
}

void MultiHttpServer::AsyncHandle::rethrowIfError() {
  // Check stored error from a previous stop() call first.
  if (_storedError) {
    std::rethrow_exception(_storedError);
  }
  std::ranges::for_each(_serverHandles, [](auto& handle) { handle.rethrowIfError(); });
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

MultiHttpServer::MultiHttpServer(HttpServerConfig cfg, Router router)
    : _stopRequested(std::make_shared<std::atomic<bool>>(false)) {
  auto threadCount = cfg.nbThreads;
  if (threadCount == 0) {
    // hardware_concurrency() returns unsigned int but is in practice <= a few thousand on the largest machines,
    // well within the 16-bit nbThreads range.
    threadCount = SafeCast<decltype(threadCount)>(std::thread::hardware_concurrency());
    if (threadCount == 0) {
      threadCount = 1;
      log::warn("Unable to detect the number of available processors for HttpServer - defaults to {}", threadCount);
    }
    log::debug("HttpServer auto-thread constructor detected hw_concurrency={}", threadCount);
  }

  _servers.reserve(static_cast<decltype(_servers)::size_type>(threadCount));

  if (threadCount > 1U && !cfg.reusePort) {
    if (cfg.port != 0) {
      // User wants to ensure that the given port will be exclusively used by this MultiHttpServer.
      // We need reusePort to be enabled for multiple threads to bind to the same port, but we can
      // verify that no other process is using the port by attempting to bind once here.
      // If the port is already in use, this will throw.
      if (!Socket{Socket::Type::StreamNonBlock}.tryBind(cfg.reusePort, cfg.port)) {
        ThrowSystemError("bind failed on this port - already in use");
      }
      // immediately close the socket again to free the port for the actual servers below.
    }

    cfg.reusePort = true;  // enforce reusePort for multi-threaded servers
    log::debug("HttpServer: Enabling reusePort for multi-threaded server");
  }

  cfg.nbThreads = 1;  // will be applied for each SingleHttpServer.
  auto& firstServer = _servers.emplace_back(std::move(cfg), std::move(router));

  firstServer._lifecycleTracker = _lifecycleTracker;
}

#ifdef AERONET_ENABLE_GLAZE
MultiHttpServer::MultiHttpServer(const std::filesystem::path& configPath) {
  auto config = detail::ParseConfigFile(configPath);
  *this = MultiHttpServer(std::move(config.server), Router(std::move(config.router)));
}

MultiHttpServer::MultiHttpServer(const std::filesystem::path& configPath, Router router) {
  auto config = detail::ParseConfigFile(configPath);
  *this = MultiHttpServer(std::move(config.server), std::move(router));
}

std::string MultiHttpServer::dumpConfig(ConfigFormat format) const {
  if (empty()) {
    throw std::logic_error("Cannot dump config from an empty MultiHttpServer");
  }
  TopLevelConfig config{.server = _servers[0]._config, .router = _servers[0]._router.config()};
  config.server.nbThreads = static_cast<std::uint16_t>(nbThreads());
  return detail::SerializeConfig(config, format);
}

void MultiHttpServer::saveConfig(const std::filesystem::path& filePath) const {
  if (empty()) {
    throw std::logic_error("Cannot save config from an empty MultiHttpServer");
  }
  const auto format = detail::DetectFormat(filePath);
  const auto content = dumpConfig(format);
  std::ofstream ofs(filePath, std::ios::binary);
  if (!ofs) {
    throw std::runtime_error("Failed to open file for writing: " + filePath.string());
  }
  ofs << content;
}
#endif

MultiHttpServer::MultiHttpServer(const MultiHttpServer& other)
    : _stopRequested(std::make_shared<std::atomic<bool>>(false)),
      _lifecycleTracker(std::make_shared<ServerLifecycleTracker>()),
      _serversAlive(std::make_shared<std::atomic<bool>>(true)) {
  if (other.isRunning()) {
    throw std::logic_error("Cannot copy-construct a running HttpServer");
  }

  _servers.reserve(other._servers.capacity());
  std::ranges::for_each(other._servers, [this](const auto& server) {
    _servers.emplace_back(server)._lifecycleTracker = _lifecycleTracker;
  });
}

MultiHttpServer::MultiHttpServer(MultiHttpServer&& other) noexcept
    : _stopRequested(std::move(other._stopRequested)),
      _lifecycleTracker(std::move(other._lifecycleTracker)),
      _servers(std::move(other._servers)),
      _probeServer(std::move(other._probeServer)),
      _internalHandle(std::move(other._internalHandle)),
      _lastHandleStopFn(std::move(other._lastHandleStopFn)),
      _lastHandleCompletion(std::move(other._lastHandleCompletion)),
      _serversAlive(std::move(other._serversAlive)) {
  std::ranges::for_each(_servers, [this](auto& server) { server._lifecycleTracker = _lifecycleTracker; });
}

MultiHttpServer& MultiHttpServer::operator=(MultiHttpServer&& other) noexcept {
  if (this != &other) [[likely]] {
    // Ensure we are not leaking running threads; stop existing group first.
    stop();

    _stopRequested = std::move(other._stopRequested);
    _lifecycleTracker = std::move(other._lifecycleTracker);
    _servers = std::move(other._servers);
    _probeServer = std::move(other._probeServer);
    _internalHandle = std::move(other._internalHandle);
    _lastHandleStopFn = std::move(other._lastHandleStopFn);
    _lastHandleCompletion = std::move(other._lastHandleCompletion);
    _serversAlive = std::move(other._serversAlive);

    std::ranges::for_each(_servers, [this](auto& server) { server._lifecycleTracker = _lifecycleTracker; });
  }
  return *this;
}

MultiHttpServer& MultiHttpServer::operator=(const MultiHttpServer& other) {
  if (this != &other) {
    if (other.isRunning()) {
      throw std::logic_error("Cannot copy-assign a running HttpServer");
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
    throw std::logic_error("Cannot access router proxy on an empty HttpServer");
  }
  return {[this](std::function<void(Router&)> fn) { this->postRouterUpdate(std::move(fn)); },
          [this]() -> Router& { return this->_servers.front()._router; }};
}

std::string MultiHttpServer::AggregatedStats::json_str() const {
  std::string out;
#ifdef AERONET_ENABLE_OPENSSL
  out.reserve(768UL * per.size());
#else
  out.reserve(256UL * per.size());
#endif
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

void MultiHttpServer::setParserErrorCallback(SingleHttpServer::ParserErrorCallback cb) {
  canSetCallbacks();
  _servers.front().setParserErrorCallback(std::move(cb));
}

void MultiHttpServer::setMetricsCallback(SingleHttpServer::MetricsCallback cb) {
  canSetCallbacks();
  _servers.front().setMetricsCallback(std::move(cb));
}

void MultiHttpServer::setExpectationHandler(SingleHttpServer::ExpectationHandler handler) {
  canSetCallbacks();
  _servers.front().setExpectationHandler(std::move(handler));
}

void MultiHttpServer::setMiddlewareMetricsCallback(MiddlewareMetricsCallback cb) {
  canSetCallbacks();
  _servers.front().setMiddlewareMetricsCallback(std::move(cb));
}

#ifdef AERONET_ENABLE_OPENSSL
void MultiHttpServer::setTlsHandshakeCallback(TlsHandshakeCallback cb) {
  canSetCallbacks();
  _servers.front().setTlsHandshakeCallback(std::move(cb));
}
#endif

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

  log_noexcept::debug("HttpServer stopping (instances={})", _servers.size());
  std::ranges::for_each(_servers, [](SingleHttpServer& server) { server.stop(); });
  if (_probeServer) {
    _probeServer->stop();
  }

  // Stop internal handle if start() was used (non-blocking API)
  if (_internalHandle) {
    _internalHandle->stop();
    _internalHandle.reset();
  }

  if (auto completion = _lastHandleCompletion.lock()) {
    completion->wait();
  }
  log_noexcept::info("HttpServer stopped");
}

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
  std::ranges::for_each(_servers, [maxWait](SingleHttpServer& server) { server.beginDrain(maxWait); });
}

bool MultiHttpServer::isDraining() const {
  return std::ranges::any_of(_servers, [](const SingleHttpServer& server) { return server.isDraining(); });
}

void MultiHttpServer::postConfigUpdate(const std::function<void(HttpServerConfig&)>& updater) {
  if (empty()) {
    throw std::logic_error("Cannot post a config update on an empty HttpServer");
  }
  std::ranges::for_each(_servers, [&updater](SingleHttpServer& server) { server.postConfigUpdate(updater); });
}

void MultiHttpServer::postRouterUpdate(std::function<void(Router&)> updater) {
  if (empty()) {
    throw std::logic_error("Cannot post a router update on an empty HttpServer");
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
    throw std::logic_error("Cannot set callbacks on an empty HttpServer");
  }
}

void MultiHttpServer::ensureNextServersBuilt() {
  if (_servers.empty()) {
    throw std::logic_error("Cannot rebuild servers on an empty HttpServer");
  }

  auto& firstServer = _servers.front();

  // On restart (when copies from the previous cycle still exist), fully
  // reinitialize the first server's listener.  After a stop cycle in multi-
  // server mode the event-loop thread may have exited via predicate before the
  // maintenance tick called closeListener(), leaving the listen socket open or
  // the event loop in a stale state.  Re-creating both here - on the single
  // main thread before any worker threads start - guarantees a clean state and
  // avoids a racy in-thread initListener() call.
  if (_servers.size() > 1UL) {
    firstServer.closeListener();
    firstServer.initListener();
  }

  firstServer.applyPendingUpdates();

#ifdef AERONET_ENABLE_OPENSSL
  // Set up shared session ticket key store on firstServer BEFORE duplication.
  // This ensures all copied servers will have the shared store when their TLS contexts are built.
  if (firstServer._config.tls.sessionTickets.enabled && !firstServer._tls.sharedTicketKeyStore) {
    firstServer._tls.sharedTicketKeyStore = std::make_shared<TlsTicketKeyStore>(
        firstServer._config.tls.sessionTickets.lifetime, firstServer._config.tls.sessionTickets.maxKeys);
    if (!firstServer._config.tls.sessionTicketKeys().empty()) {
      firstServer._tls.sharedTicketKeyStore->loadStaticKeys(firstServer._config.tls.sessionTicketKeys());
    }
  }
#endif

  const auto targetCount = _servers.capacity();

  // Invalidate the old serversAlive guard before destroying any server instances.
  // Any stale stop callback that captured the previous serversAlive will see false
  // and skip dereferencing raw server pointers.
  _serversAlive->store(false, std::memory_order_release);

  _servers.resize(1UL);

  // Create a fresh guard for the next start cycle.
  _serversAlive = std::make_shared<std::atomic<bool>>(true);

#ifdef AERONET_MACOS
  // macOS SO_REUSEPORT does not load balance loopback traffic - the kernel routes
  // all connections to the most recently bound socket. Instead, share a single
  // listen fd across all threads; each thread registers EVFILT_READ on the fd in
  // its own kqueue. The kernel serialises accept() so only one thread wakes per
  // connection, providing natural thundering-herd-free distribution.
  const NativeHandle sharedListenFd = firstServer._listenSocket.fd();
#else
  // Linux: each copy creates its own socket+bind via SO_REUSEPORT for true
  // kernel-level load balancing with zero contention on accept().
  const NativeHandle sharedListenFd = kInvalidHandle;
#endif
  while (_servers.size() < targetCount) {
    auto& nextServer = _servers.emplace_back(firstServer, sharedListenFd);
    nextServer._lifecycleTracker = _lifecycleTracker;
  }

  // (Re)build the dedicated probe listener and its ProbeState when builtinProbes.dedicatedPort is configured.
  // Called from ensureNextServersBuilt() once the worker event loops (and their resolved port) exist.
  buildProbeServerIfEnabled();
}

void MultiHttpServer::buildProbeServerIfEnabled() {
  // Drop any listener from a previous run cycle before (re)building. Resetting it also releases the previous
  // ProbeState, which is owned by the route-handler lambdas captured in the listener's router (nowhere else).
  _probeServer.reset();

  const BuiltinProbesConfig& probesCfg = _servers.front()._config.builtinProbes;
  if (!probesCfg.enabled || probesCfg.dedicatedPort == 0) {
    return;  // Inline probes (or no probes): nothing dedicated to build.
  }

  // The workers' listening port is already resolved (bound at construction), even if it was ephemeral (0) in the
  // user config. A dedicated probe listener sharing that port would be meaningless (and unbindable).
  const uint16_t appPort = _servers.front().port();
  if (probesCfg.dedicatedPort == appPort) {
    throw std::invalid_argument("builtinProbes.dedicatedPort must differ from the server listening port");
  }

  // Build the aggregated worker view captured by the probe route handlers.
  auto probeState = std::make_shared<ProbeState>();
  probeState->livenessThresholdNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(probesCfg.livenessStaleThreshold).count();
  probeState->workers.reserve(_servers.size());
  for (SingleHttpServer& server : _servers) {
    probeState->workers.push_back(&server._lifecycle);
  }

  // A minimal, plaintext HTTP/1.1 listener dedicated to the probe endpoints. We start from a default config (no TLS,
  // no HTTPS redirect, builtin probes disabled) so nothing on this port can block or redirect a Kubernetes httpGet
  // probe, then trim it to the bare minimum: it sees roughly one request per second, runs no application handlers and
  // only ever emits three tiny fixed responses. Every knob below shrinks its idle memory / CPU footprint without
  // hurting probe responsiveness - an incoming connection always wakes the poll immediately, whatever pollInterval is.
  HttpServerConfig probeCfg;
  probeCfg.port = probesCfg.dedicatedPort;
  probeCfg.nbThreads = 1;
  probeCfg.enableKeepAlive = false;                 // probes are single-shot; never hold an idle connection open
  probeCfg.maxCachedConnections = 1U;               // one recycled ConnectionState is plenty (requests never overlap)
  probeCfg.maxAcceptBatchSize = 4U;                 // a prober never opens a burst of connections
  probeCfg.maxHeaderBytes = 1024U;                  // a probe request is a few hundred bytes at most (floor is 128)
  probeCfg.maxBodyBytes = 1024U;                    // probes carry no body; bound anything unexpected sent to this port
  probeCfg.pollInterval = std::chrono::seconds{2};  // almost always idle: block in poll for seconds, not ms
  probeCfg.pollIntervalMaxFactor = 4.0F;            // back off up to ~8s between wakeups while idle
  probeCfg.builtinProbes.enabled = false;
  _probeServer = std::make_unique<SingleHttpServer>(std::move(probeCfg));
  SingleHttpServer& probeServer = *_probeServer;

  // Register the three probe routes reading the aggregated worker view (mirrors the inline probe responses).
  const auto liveness = probesCfg.livenessPath();
  const auto readiness = probesCfg.readinessPath();
  const auto startup = probesCfg.startupPath();
  probeServer._router.setPath(http::Method::GET, liveness, [probeState](const HttpRequest& req) {
    const bool live = probeState->live();
    return req.makeResponse(live ? http::StatusCodeOK : http::StatusCodeServiceUnavailable, live ? "OK" : "Unhealthy");
  });
  probeServer._router.setPath(http::Method::GET, readiness, [probeState](const HttpRequest& req) {
    const bool ready = probeState->ready();
    return req.makeResponse(ready ? http::StatusCodeOK : http::StatusCodeServiceUnavailable,
                            ready ? "OK" : "Not Ready");
  });
  probeServer._router.setPath(http::Method::GET, startup, [probeState](const HttpRequest& req) {
    const bool started = probeState->started();
    return req.makeResponse(started ? http::StatusCodeOK : http::StatusCodeServiceUnavailable,
                            started ? "OK" : "Starting");
  });

  log::debug("HttpServer dedicated probe listener bound on port :{}", probeServer.port());
}

vector<SingleHttpServer*> MultiHttpServer::collectServerPointers() {
  // Every background-thread server: the workers plus, when configured, the dedicated probe listener.
  vector<SingleHttpServer*> serverPtrs;
  serverPtrs.reserve(_servers.size() + (_probeServer ? 1U : 0U));
  for (SingleHttpServer& server : _servers) {
    serverPtrs.push_back(&server);
  }
  if (_probeServer) {
    serverPtrs.push_back(_probeServer.get());
  }
  return serverPtrs;
}

void MultiHttpServer::runBlocking(std::function<bool()> predicate, std::string_view modeLabel) {
  if (_servers.empty()) {
    throw std::logic_error("Cannot run an empty HttpServer");
  }

  if (isRunning()) {
    throw std::logic_error("HttpServer already started");
  }

  // Use a local AsyncHandle to manage the servers.
  // We do NOT store it in _internalHandle to avoid race conditions with stop().
  // stop() will signal _stopRequested and wait for us via _lastHandleStopFn.
  AsyncHandle handle = startDetachedInternal(std::move(predicate), {});

  const bool started = _lifecycleTracker->waitUntilAnyRunning(*_stopRequested);
  if (!_stopRequested->load(std::memory_order_relaxed) && started) {
    _lifecycleTracker->waitUntilAllStopped(*_stopRequested);
  }

  log::info("HttpServer {}{}stopped", modeLabel, modeLabel.empty() ? "" : " ");

  // handle goes out of scope here, stopping and joining all threads.
}

MultiHttpServer::AsyncHandle MultiHttpServer::startDetachedInternal(std::function<bool()> extraStopCondition,
                                                                    const std::stop_token& externalStopToken) {
  if (_servers.empty()) {
    throw std::logic_error("Cannot start an empty HttpServer");
  }
  if (isRunning()) {
    throw std::logic_error("HttpServer already started");
  }

  _lifecycleTracker->clear();

  // Create the remaining servers.
  ensureNextServersBuilt();

  _stopRequested->store(false, std::memory_order_relaxed);

  log::debug("HttpServer starting with {} thread(s) on port :{}", _servers.size(), port());

  if (!extraStopCondition) {
    extraStopCondition = [] { return false; };
  }

  // Workers + optional dedicated probe listener: launched, stopped and joined uniformly.
  const auto serverPtrs = collectServerPointers();

  vector<SingleHttpServer::AsyncHandle> serverHandles;
  serverHandles.reserve(serverPtrs.size());

  auto lifecycleTracker = _lifecycleTracker;

  auto serversAlive = _serversAlive;
  auto stopCallback = std::make_shared<std::function<void()>>([serverPtrs, serversAlive]() noexcept {
    if (serversAlive && serversAlive->load(std::memory_order_acquire)) {
      std::ranges::for_each(serverPtrs, [](SingleHttpServer* srv) { srv->stop(); });
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

  // Each worker/probe thread stops when the shared flag is set, or when the caller's extra stop condition fires
  // (in which case it also latches the shared flag and triggers the shared stop callback for the whole group).
  auto makeStopPredicate = [this, &extraStopCondition, &stopCallback, &lifecycleTracker]() {
    std::atomic<bool>* stopRequested = _stopRequested.get();
    auto threadExtraStop = extraStopCondition;
    return [stopRequested, threadExtraStop, stopCallback, lifecycleTracker]() {
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
    };
  };

  // Launch threads (each captures a stable pointer to its SingleHttpServer). The dedicated probe listener, when
  // present, is the last entry and runs on its own thread, so its availability is never affected by worker load.
  for (SingleHttpServer* server : serverPtrs) {
    serverHandles.push_back(server->startDetachedAndStopWhen(makeStopPredicate()));
  }
  log::info("HttpServer started with {} thread(s) on port :{}", _servers.size(), port());

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
    agg.total.tlsHandshakesFull += st.tlsHandshakesFull;
    agg.total.tlsHandshakesResumed += st.tlsHandshakesResumed;
    agg.total.tlsHandshakesFailed += st.tlsHandshakesFailed;
    agg.total.tlsHandshakesRejectedConcurrency += st.tlsHandshakesRejectedConcurrency;
    agg.total.tlsHandshakesRejectedRateLimit += st.tlsHandshakesRejectedRateLimit;
    for (const auto& [key, value] : st.tlsAlpnDistribution) {
      auto it = std::ranges::find(agg.total.tlsAlpnDistribution, key, &std::pair<std::string, uint64_t>::first);
      if (it != agg.total.tlsAlpnDistribution.end()) {
        it->second += value;
      } else {
        agg.total.tlsAlpnDistribution.emplace_back(key, value);
      }
    }
    for (const auto& [key, value] : st.tlsHandshakeFailureReasons) {
      auto it = std::ranges::find(agg.total.tlsHandshakeFailureReasons, key, &std::pair<std::string, uint64_t>::first);
      if (it != agg.total.tlsHandshakeFailureReasons.end()) {
        it->second += value;
      } else {
        agg.total.tlsHandshakeFailureReasons.emplace_back(key, value);
      }
    }
    for (const auto& [key, value] : st.tlsVersionCounts) {
      auto it = std::ranges::find(agg.total.tlsVersionCounts, key, &std::pair<std::string, uint64_t>::first);
      if (it != agg.total.tlsVersionCounts.end()) {
        it->second += value;
      } else {
        agg.total.tlsVersionCounts.emplace_back(key, value);
      }
    }
    for (const auto& [key, value] : st.tlsCipherCounts) {
      auto it = std::ranges::find(agg.total.tlsCipherCounts, key, &std::pair<std::string, uint64_t>::first);
      if (it != agg.total.tlsCipherCounts.end()) {
        it->second += value;
      } else {
        agg.total.tlsCipherCounts.emplace_back(key, value);
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
