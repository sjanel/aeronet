#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-lifecycle-tracker.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tracing/tracer.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-encoder.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#endif

#ifdef AERONET_ENABLE_ZSTD
#include "aeronet/zstd-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-context.hpp"
#endif

namespace aeronet {

namespace {

class LifecycleTrackerGuard {
 public:
  explicit LifecycleTrackerGuard(std::weak_ptr<ServerLifecycleTracker> tracker) : _tracker(std::move(tracker)) {
    if (auto locked = _tracker.lock()) {
      locked->notifyServerRunning();
    }
  }

  ~LifecycleTrackerGuard() {
    if (auto locked = _tracker.lock()) {
      locked->notifyServerStopped();
    }
  }

  LifecycleTrackerGuard(const LifecycleTrackerGuard&) = delete;
  LifecycleTrackerGuard& operator=(const LifecycleTrackerGuard&) = delete;
  LifecycleTrackerGuard(LifecycleTrackerGuard&&) = delete;
  LifecycleTrackerGuard& operator=(LifecycleTrackerGuard&&) = delete;

 private:
  std::weak_ptr<ServerLifecycleTracker> _tracker;
};
}  // namespace

SingleHttpServer::AsyncHandle::AsyncHandle(std::jthread thread, std::shared_ptr<std::exception_ptr> error)
    : _thread(std::move(thread)), _error(std::move(error)) {}

SingleHttpServer::AsyncHandle::~AsyncHandle() { stop(); }

void SingleHttpServer::AsyncHandle::stop() noexcept {
  if (_thread.joinable()) {
    _thread.request_stop();
    _thread.join();
  }
}

void SingleHttpServer::AsyncHandle::rethrowIfError() {
  if (_error && *_error) {
    std::rethrow_exception(*_error);
  }
}

SingleHttpServer::SingleHttpServer(HttpServerConfig config, RouterConfig routerConfig)
    : _config(std::move(config)),
      _listenSocket(Socket::Type::StreamNonBlock),
      _eventLoop(_config.pollInterval),
      _router(std::move(routerConfig)),
      _encodingSelector(_config.compression),
      _telemetry(_config.telemetry) {
  initListener();
}

SingleHttpServer::SingleHttpServer(HttpServerConfig cfg, Router router)
    : _config(std::move(cfg)),
      _listenSocket(Socket::Type::StreamNonBlock),
      _eventLoop(_config.pollInterval),
      _router(std::move(router)),
      _encodingSelector(_config.compression),
      _telemetry(_config.telemetry) {
  initListener();
}

SingleHttpServer::SingleHttpServer(const SingleHttpServer& other)
    : _config(other._config),
      _listenSocket(Socket::Type::StreamNonBlock),
      _isInMultiHttpServer(other._isInMultiHttpServer),
      _eventLoop(_config.pollInterval),
      _router(other._router),
      _encodingSelector(_config.compression),
      _parserErrCb(other._parserErrCb),
      _metricsCb(other._metricsCb),
      _middlewareMetricsCb(other._middlewareMetricsCb),
#ifdef AERONET_ENABLE_OPENSSL
      _tlsHandshakeCb(other._tlsHandshakeCb),
#endif
      _expectationHandler(other._expectationHandler),
      _pendingConfigUpdates(other._pendingConfigUpdates),
      _pendingRouterUpdates(other._pendingRouterUpdates),
      _telemetry(_config.telemetry)
#ifdef AERONET_ENABLE_OPENSSL
      ,
      _sharedTicketKeyStore(other._sharedTicketKeyStore)
#endif
{
  if (!other._lifecycle.isIdle()) {
    throw std::logic_error("Cannot copy-construct from a running SingleHttpServer");
  }

  _hasPendingConfigUpdates.store(other._hasPendingConfigUpdates.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
  _hasPendingRouterUpdates.store(other._hasPendingRouterUpdates.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);

  initListener();
}

SingleHttpServer& SingleHttpServer::operator=(const SingleHttpServer& other) {
  if (this != &other) {
    if (!other._lifecycle.isIdle()) {
      throw std::logic_error("Cannot copy-assign from a running SingleHttpServer");
    }

    stop();

    const bool wasInMulti = _isInMultiHttpServer;
    auto lifecycleTracker = _lifecycleTracker;

    *this = SingleHttpServer(other);

    _isInMultiHttpServer = wasInMulti;
    _lifecycleTracker = std::move(lifecycleTracker);
  }
  return *this;
}

// NOLINTNEXTLINE(bugprone-exception-escape,performance-noexcept-move-constructor)
SingleHttpServer::SingleHttpServer(SingleHttpServer&& other)
    : _stats(std::exchange(other._stats, {})),
      _config(std::move(other._config)),
      _listenSocket(std::move(other._listenSocket)),
      _isInMultiHttpServer(other._isInMultiHttpServer),
      _eventLoop(std::move(other._eventLoop)),
      _lifecycle(std::move(other._lifecycle)),
      _router(std::move(other._router)),
      _activeConnectionsMap(std::move(other._activeConnectionsMap)),
      _cachedConnections(std::move(other._cachedConnections)),
      _encoders(std::move(other._encoders)),
      _encodingSelector(std::move(other._encodingSelector)),
      _parserErrCb(std::move(other._parserErrCb)),
      _metricsCb(std::move(other._metricsCb)),
      _middlewareMetricsCb(std::move(other._middlewareMetricsCb)),
#ifdef AERONET_ENABLE_OPENSSL
      _tlsHandshakeCb(std::move(other._tlsHandshakeCb)),
#endif
      _expectationHandler(std::move(other._expectationHandler)),
      _tmpBuffer(std::move(other._tmpBuffer)),
      _pendingConfigUpdates(std::move(other._pendingConfigUpdates)),
      _pendingRouterUpdates(std::move(other._pendingRouterUpdates)),
      _telemetry(std::move(other._telemetry)),
      _internalHandle(std::move(other._internalHandle)),
      _lifecycleTracker(std::move(other._lifecycleTracker))
#ifdef AERONET_ENABLE_OPENSSL
      ,
      _tlsCtxHolder(std::move(other._tlsCtxHolder)),
      _tlsMetrics(std::move(other._tlsMetrics))
#endif
{
  if (!_lifecycle.isIdle()) {
    throw std::logic_error("Cannot move-construct a running SingleHttpServer");
  }

  // transfer pending updates state; mutex remains with each instance (do not move mutex)
  _hasPendingConfigUpdates.store(other._hasPendingConfigUpdates.exchange(false), std::memory_order_release);
  _hasPendingRouterUpdates.store(other._hasPendingRouterUpdates.exchange(false), std::memory_order_release);
  other._lifecycle.reset();
}

// NOLINTNEXTLINE(bugprone-exception-escape,performance-noexcept-move-constructor)
SingleHttpServer& SingleHttpServer::operator=(SingleHttpServer&& other) {
  if (this != &other) {
    stop();

    if (!other._lifecycle.isIdle()) {
      other.stop();
      throw std::logic_error("Cannot move-assign from a running SingleHttpServer");
    }
    _stats = std::exchange(other._stats, {});
    _config = std::move(other._config);
    _listenSocket = std::move(other._listenSocket);
    _isInMultiHttpServer = other._isInMultiHttpServer;
    _eventLoop = std::move(other._eventLoop);
    _lifecycle = std::move(other._lifecycle);
    _router = std::move(other._router);
    _activeConnectionsMap = std::move(other._activeConnectionsMap);
    _cachedConnections = std::move(other._cachedConnections);
    _encoders = std::move(other._encoders);
    _encodingSelector = std::move(other._encodingSelector);
    _parserErrCb = std::move(other._parserErrCb);
    _metricsCb = std::move(other._metricsCb);
    _middlewareMetricsCb = std::move(other._middlewareMetricsCb);
#ifdef AERONET_ENABLE_OPENSSL
    _tlsHandshakeCb = std::move(other._tlsHandshakeCb);
#endif
    _expectationHandler = std::move(other._expectationHandler);
    _tmpBuffer = std::move(other._tmpBuffer);
    _pendingConfigUpdates = std::move(other._pendingConfigUpdates);
    _pendingRouterUpdates = std::move(other._pendingRouterUpdates);
    _telemetry = std::move(other._telemetry);
    _internalHandle = std::move(other._internalHandle);
    _lifecycleTracker = std::move(other._lifecycleTracker);

#ifdef AERONET_ENABLE_OPENSSL
    _tlsCtxHolder = std::move(other._tlsCtxHolder);
    _tlsMetrics = std::move(other._tlsMetrics);
#endif
    // transfer pending updates state; keep mutex per-instance
    _hasPendingConfigUpdates.store(other._hasPendingConfigUpdates.exchange(false), std::memory_order_release);
    _hasPendingRouterUpdates.store(other._hasPendingRouterUpdates.exchange(false), std::memory_order_release);

    other._lifecycle.reset();
  }
  return *this;
}

SingleHttpServer::~SingleHttpServer() { stop(); }

// Performs full listener initialization (RAII style) so that port() is valid immediately after construction.
// Steps (in order) and rationale / failure characteristics:
//   1. socket(AF_INET, SOCK_STREAM, 0)
//        - Expected to succeed under normal conditions. Failure indicates resource exhaustion
//          (EMFILE per-process fd limit, ENFILE system-wide, ENOBUFS/ENOMEM) or misconfiguration (rare EACCES).
//   2. setsockopt(SO_REUSEADDR)
//        - Practically infallible unless programming error (EINVAL) or extreme memory pressure (ENOMEM).
//          Mandatory to allow rapid restart after TIME_WAIT collisions.
//   3. setsockopt(SO_REUSEPORT) (optional best-effort)
//        - Enabled only if cfg.reusePort. May fail on older kernels (EOPNOTSUPP/EINVAL) -> logged as warning only,
//          not fatal. This provides horizontal scaling (multi-reactor) when supported.
//   4. bind()
//        - Most common legitimate failure point: EADDRINUSE when user supplies a fixed port already in use, or
//          EACCES for privileged ports (<1024) without CAP_NET_BIND_SERVICE. With cfg.port == 0 (ephemeral) the
//          collision probability is effectively eliminated; failures then usually imply resource exhaustion or
//          misconfiguration. Chosen early to surface environmental issues promptly.
//   5. listen()
//        - Rarely fails after successful bind; would signal extreme resource pressure or unexpected kernel state.
//   6. getsockname() (only if ephemeral port requested)
//        - Retrieves the kernel-assigned port so tests / orchestrators can read it deterministically. Extremely
//          reliable; failure would imply earlier descriptor issues (EBADF) which would already have thrown.
//   7. fcntl(F_GETFL/F_SETFL O_NONBLOCK)
//        - Should not fail unless EBADF or EINVAL (programming error). Makes accept + IO non-blocking for epoll ET.
//   8. epoll add (via EventLoop::add)
//        - Registers the listening fd for readiness notifications. Possible errors: ENOMEM/ENOSPC (resource limits),
//          EBADF (logic bug), EEXIST (should not happen). Treated as fatal.
//
// Exception Semantics:
//   - On any fatal failure the constructor throws std::runtime_error after closing the partially created _listenFd.
//   - This yields strong exception safety: either you have a fully registered, listening server instance or no
//     observable side effects. Users relying on non-throwing control flow can wrap construction in a factory that
//     maps exceptions to error codes / expected<>.
//
// Operational Expectations:
//   - In a nominal environment using an ephemeral port (cfg.port == 0), the probability of an exception is ~0 unless
//     the process hits fd limits or severe memory pressure. Fixed ports may legitimately throw due to EADDRINUSE.
//   - Using ephemeral ports in tests removes port collision flakiness across machines / CI runs.
void SingleHttpServer::initListener() {
  if (_config.nbThreads > 1U) {
    throw std::invalid_argument("SingleHttpServer cannot be configured with nbThreads > 1");
  }
  _config.validate();

  if (!_listenSocket) {
    _listenSocket = Socket(Socket::Type::StreamNonBlock);
    _eventLoop = EventLoop(_config.pollInterval);
  }

#ifdef AERONET_ENABLE_OPENSSL
  // Initialize TLS context if requested (OpenSSL build).
  if (_config.tls.enabled) {
    _tlsCtxHolder = std::make_shared<TlsContext>(_config.tls, _sharedTicketKeyStore);
  }
#endif

  _listenSocket.bindAndListen(_config.reusePort, _config.tcpNoDelay, _config.port);

  _eventLoop.addOrThrow(EventLoop::EventFd{_listenSocket.fd(), EventIn});
  _eventLoop.addOrThrow(EventLoop::EventFd{_lifecycle.wakeupFd.fd(), EventIn});

  // Pre-allocate encoders (one per supported format if available at compile time) so per-response paths can reuse them.
  createEncoders();
}

void SingleHttpServer::prepareRun() {
  if (_lifecycle.isActive()) {
    throw std::logic_error("Server is already running");
  }
  if (!_listenSocket) {
    initListener();
  }
  if (!_isInMultiHttpServer) {
    // In MultiHttpServer, logging is done at that level instead.
    log::info("Server running on port :{}", port());
  }

  // Register builtin probes handlers if enabled in config
  registerBuiltInProbes();
}

void SingleHttpServer::run() {
  prepareRun();
  _lifecycle.enterRunning();
  LifecycleTrackerGuard trackerGuard(_lifecycleTracker);
  while (_lifecycle.isActive()) {
    eventLoop();
  }
  _lifecycle.reset();
}

void SingleHttpServer::runUntil(const std::function<bool()>& predicate) {
  prepareRun();
  _lifecycle.enterRunning();
  LifecycleTrackerGuard trackerGuard(_lifecycleTracker);
  while (_lifecycle.isActive() && !predicate()) {
    eventLoop();
  }
  if (_lifecycle.isActive()) {
    _lifecycle.reset();
  }
}

void SingleHttpServer::start() { _internalHandle = startDetached(); }

SingleHttpServer::AsyncHandle SingleHttpServer::startDetached() {
  auto errorPtr = std::make_shared<std::exception_ptr>();

  return {std::jthread([this, errorPtr](const std::stop_token& st) {
            try {
              runUntil([&st]() { return st.stop_requested(); });
            } catch (...) {
              *errorPtr = std::current_exception();
            }
          }),
          std::move(errorPtr)};
}

SingleHttpServer::AsyncHandle SingleHttpServer::startDetachedAndStopWhen(std::function<bool()> predicate) {
  auto errorPtr = std::make_shared<std::exception_ptr>();

  return {std::jthread([this, pred = std::move(predicate), errorPtr](const std::stop_token& st) {
            try {
              runUntil([&st, &pred]() { return st.stop_requested() || pred(); });
            } catch (...) {
              *errorPtr = std::current_exception();
            }
          }),
          std::move(errorPtr)};
}

SingleHttpServer::AsyncHandle SingleHttpServer::startDetachedWithStopToken(std::stop_token token) {
  auto errorPtr = std::make_shared<std::exception_ptr>();

  return {std::jthread([this, token = std::move(token), errorPtr](const std::stop_token& st) {
            try {
              runUntil([&st, &token]() { return st.stop_requested() || token.stop_requested(); });
            } catch (...) {
              *errorPtr = std::current_exception();
            }
          }),
          std::move(errorPtr)};
}

void SingleHttpServer::stop() noexcept {
  closeListener();
  if (_lifecycle.exchangeStopping() == internal::Lifecycle::State::Running) {
    log::debug("Stopping server");

    // Stop internal handle if start() was used (non-blocking API)
    _internalHandle.stop();

    _lifecycle.reset();
    log::debug("Stopped server");
  }
}

void SingleHttpServer::beginDrain(std::chrono::milliseconds maxWait) noexcept {
  if (!_lifecycle.isActive() || _lifecycle.isStopping()) {
    return;
  }

  const bool hasDeadline = maxWait.count() > 0;
  const auto deadline =
      hasDeadline ? std::chrono::steady_clock::now() + maxWait : std::chrono::steady_clock::time_point{};

  if (_lifecycle.isDraining()) {
    if (hasDeadline) {
      _lifecycle.shrinkDeadline(deadline);
    }
    return;
  }

  if (!_activeConnectionsMap.empty()) {
    log::info("Initiating graceful drain (connections={})", _activeConnectionsMap.size());
  }

  _lifecycle.enterDraining(deadline, hasDeadline);
  // Keep listener open during drain to allow health probes to connect and receive 503 status.
  // Regular connections will still be accepted but will receive Connection: close headers.
}

void SingleHttpServer::registerBuiltInProbes() {
  if (!_config.builtinProbes.enabled) {
    return;
  }

  // Liveness: Always returns 200 OK if the server is responding.
  // Indicates the application is alive (not deadlocked/crashed).
  _router.setPath(http::Method::GET, _config.builtinProbes.livenessPath(),
                  [](const HttpRequest&) { return HttpResponse("OK\n"); });

  // Readiness: Returns 200 when ready to serve traffic, 503 during drain.
  // Used by load balancers to determine if instance should receive traffic.
  _router.setPath(http::Method::GET, _config.builtinProbes.readinessPath(), [this](const HttpRequest&) {
    HttpResponse resp(http::StatusCodeOK);
    if (_lifecycle.ready()) {
      resp.body("OK\n");
    } else {
      resp.status(http::StatusCodeServiceUnavailable);
      resp.body("Not Ready\n");
    }
    return resp;
  });

  // Startup: Returns 200 once server is running (listener active).
  // Note: Since aeronet has no separate initialization phase, this is essentially
  // equivalent to liveness. Provided for Kubernetes compatibility where startup
  // probes can have different timeout/period settings for slow-starting applications.
  _router.setPath(http::Method::GET, _config.builtinProbes.startupPath(), [this](const HttpRequest&) {
    HttpResponse resp(http::StatusCodeOK);
    if (_lifecycle.started()) {
      resp.body("OK\n");
    } else {
      resp.status(http::StatusCodeServiceUnavailable);
      resp.body("Starting\n");
    }
    return resp;
  });
}

void SingleHttpServer::createEncoders() {
#ifdef AERONET_ENABLE_ZLIB
  _encoders[static_cast<std::size_t>(Encoding::gzip)] =
      std::make_unique<ZlibEncoder>(ZStreamRAII::Variant::gzip, _config.compression);
  _encoders[static_cast<std::size_t>(Encoding::deflate)] =
      std::make_unique<ZlibEncoder>(ZStreamRAII::Variant::deflate, _config.compression);
#endif
#ifdef AERONET_ENABLE_ZSTD
  _encoders[static_cast<std::size_t>(Encoding::zstd)] = std::make_unique<ZstdEncoder>(_config.compression);
#endif
#ifdef AERONET_ENABLE_BROTLI
  _encoders[static_cast<std::size_t>(Encoding::br)] = std::make_unique<BrotliEncoder>(_config.compression);
#endif
}

}  // namespace aeronet