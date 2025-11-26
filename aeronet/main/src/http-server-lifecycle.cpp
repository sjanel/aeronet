#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

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
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/errno_throw.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-lifecycle-tracker.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tracing/tracer.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZSTD
#include "aeronet/zstd-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-context.hpp"
#endif

namespace aeronet {

namespace {
constexpr int kListenSocketType = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;

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

HttpServer::AsyncHandle::AsyncHandle(std::jthread thread, std::shared_ptr<std::exception_ptr> error)
    : _thread(std::move(thread)), _error(std::move(error)) {}

HttpServer::AsyncHandle::~AsyncHandle() { stop(); }

void HttpServer::AsyncHandle::stop() noexcept {
  if (_thread.joinable()) {
    _thread.request_stop();
    _thread.join();
  }
}

void HttpServer::AsyncHandle::rethrowIfError() {
  if (_error && *_error) {
    std::rethrow_exception(*_error);
  }
}

HttpServer::HttpServer(HttpServerConfig config, RouterConfig routerConfig)
    : _config(std::move(config)),
      _listenSocket(kListenSocketType),
      _eventLoop(_config.pollInterval),
      _router(std::move(routerConfig)),
      _encodingSelector(_config.compression),
      _telemetry(_config.telemetry) {
  init();
}

HttpServer::HttpServer(HttpServerConfig cfg, Router router)
    : _config(std::move(cfg)),
      _listenSocket(kListenSocketType),
      _eventLoop(_config.pollInterval),
      _router(std::move(router)),
      _encodingSelector(_config.compression),
      _telemetry(_config.telemetry) {
  init();
}

HttpServer::HttpServer(const HttpServer& other)
    : _config(other._config),
      _listenSocket(kListenSocketType),
      _isInMultiHttpServer(other._isInMultiHttpServer),
      _eventLoop(_config.pollInterval),
      _router(other._router),
      _encodingSelector(_config.compression),
      _parserErrCb(other._parserErrCb),
      _metricsCb(other._metricsCb),
      _middlewareMetricsCb(other._middlewareMetricsCb),
      _expectationHandler(other._expectationHandler),
      _pendingConfigUpdates(other._pendingConfigUpdates),
      _pendingRouterUpdates(other._pendingRouterUpdates),
      _telemetry(_config.telemetry) {
  if (!other._lifecycle.isIdle()) {
    throw std::logic_error("Cannot copy-construct from a running HttpServer");
  }

  _hasPendingConfigUpdates.store(other._hasPendingConfigUpdates.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
  _hasPendingRouterUpdates.store(other._hasPendingRouterUpdates.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);

  init();
}

// NOLINTNEXTLINE(bugprone-exception-escape,performance-noexcept-move-constructor)
HttpServer::HttpServer(HttpServer&& other)
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
      _tlsMetrics(std::move(other._tlsMetrics)),
      _tlsMetricsExternal(std::exchange(other._tlsMetricsExternal, {}))
#endif
{
  if (!_lifecycle.isIdle()) {
    throw std::logic_error("Cannot move-construct a running HttpServer");
  }

  // transfer pending updates state; mutex remains with each instance (do not move mutex)
  _hasPendingConfigUpdates.store(other._hasPendingConfigUpdates.exchange(false), std::memory_order_release);
  _hasPendingRouterUpdates.store(other._hasPendingRouterUpdates.exchange(false), std::memory_order_release);
  other._lifecycle.reset();
}

// NOLINTNEXTLINE(bugprone-exception-escape,performance-noexcept-move-constructor)
HttpServer& HttpServer::operator=(HttpServer&& other) {
  if (this != &other) {
    stop();

    if (!other._lifecycle.isIdle()) {
      other.stop();
      throw std::logic_error("Cannot move-assign from a running HttpServer");
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
    _tlsMetricsExternal = std::exchange(other._tlsMetricsExternal, {});
#endif
    // transfer pending updates state; keep mutex per-instance
    _hasPendingConfigUpdates.store(other._hasPendingConfigUpdates.exchange(false), std::memory_order_release);
    _hasPendingRouterUpdates.store(other._hasPendingRouterUpdates.exchange(false), std::memory_order_release);

    other._lifecycle.reset();
  }
  return *this;
}

HttpServer::~HttpServer() { stop(); }

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
void HttpServer::init() {
  _config.validate();

  if (!_listenSocket) {
    _listenSocket = Socket(kListenSocketType);
    _eventLoop = EventLoop(_config.pollInterval);
  }

  const int listenFd = _listenSocket.fd();

  // Initialize TLS context if requested (OpenSSL build).
  if (_config.tls.enabled) {
#ifdef AERONET_ENABLE_OPENSSL
    // Allocate TlsContext on the heap so its address remains stable even if HttpServer is moved.
    // (See detailed rationale in header next to _tlsCtxHolder.)
    _tlsCtxHolder = std::make_unique<TlsContext>(_config.tls, &_tlsMetricsExternal);
#else
    throw std::invalid_argument("aeronet built without OpenSSL support but TLS configuration provided");
#endif
  }
  static constexpr int enable = 1;
  if (::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) == -1) {
    throw_errno("setsockopt(SO_REUSEADDR) failed");
  }
  if (_config.reusePort && ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable)) == -1) {
    throw_errno("setsockopt(SO_REUSEPORT) failed");
  }
  if (_config.tcpNoDelay && ::setsockopt(listenFd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) == -1) {
    throw_errno("setsockopt(TCP_NODELAY) failed");
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(_config.port);
  if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
    throw_errno("bind failed");
  }
  if (::listen(listenFd, SOMAXCONN) == -1) {
    throw_errno("listen failed");
  }
  if (_config.port == 0) {
    sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&actual), &alen) == -1) {
      throw_errno("getsockname failed");
    }
    _config.port = ntohs(actual.sin_port);
  }
  _eventLoop.addOrThrow(EventLoop::EventFd{listenFd, EventIn});
  _eventLoop.addOrThrow(EventLoop::EventFd{_lifecycle.wakeupFd.fd(), EventIn});

  // Pre-allocate encoders (one per supported format if available at compile time) so per-response paths can reuse them.
  createEncoders();
}

void HttpServer::prepareRun() {
  if (_lifecycle.isActive()) {
    throw std::logic_error("Server is already running");
  }
  if (!_listenSocket) {
    init();
  }
  if (!_isInMultiHttpServer) {
    // In MultiHttpServer, logging is done at that level instead.
    log::info("Server running on port :{}", port());
  }

  // Register builtin probes handlers if enabled in config
  registerBuiltInProbes();
}

void HttpServer::run() {
  prepareRun();
  _lifecycle.enterRunning();
  LifecycleTrackerGuard trackerGuard(_lifecycleTracker);
  while (_lifecycle.isActive()) {
    eventLoop();
  }
  _lifecycle.reset();
}

void HttpServer::runUntil(const std::function<bool()>& predicate) {
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

void HttpServer::start() { _internalHandle.emplace(startDetached()); }

HttpServer::AsyncHandle HttpServer::startDetached() {
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

HttpServer::AsyncHandle HttpServer::startDetachedAndStopWhen(std::function<bool()> predicate) {
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

HttpServer::AsyncHandle HttpServer::startDetachedWithStopToken(std::stop_token token) {
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

void HttpServer::stop() noexcept {
  closeListener();
  if (_lifecycle.exchangeStopping() == internal::Lifecycle::State::Running) {
    log::debug("Stopping server");

    // Stop internal handle if start() was used (non-blocking API)
    if (_internalHandle) {
      _internalHandle->stop();
      _internalHandle.reset();
    }
    _lifecycle.reset();
    log::debug("Stopped server");
  }
}

void HttpServer::beginDrain(std::chrono::milliseconds maxWait) noexcept {
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
  closeListener();
}

void HttpServer::registerBuiltInProbes() {
  if (!_config.builtinProbes.enabled) {
    return;
  }

  // liveness: lightweight, should not depend on external systems
  _router.setPath(http::Method::GET, _config.builtinProbes.livenessPath(),
                  [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK).body("OK\n"); });

  // readiness: reflects lifecycle.ready
  _router.setPath(http::Method::GET, _config.builtinProbes.readinessPath(), [this](const HttpRequest&) {
    HttpResponse resp(http::StatusCodeOK);
    if (_lifecycle.ready.load(std::memory_order_relaxed)) {
      resp.body("OK\n");
    } else {
      resp.status(http::StatusCodeServiceUnavailable);
      resp.body("Not Ready\n");
    }
    return resp;
  });

  // startup: reflects lifecycle.started
  _router.setPath(http::Method::GET, _config.builtinProbes.startupPath(), [this](const HttpRequest&) {
    HttpResponse resp(http::StatusCodeOK);
    if (_lifecycle.started.load(std::memory_order_relaxed)) {
      resp.body("OK\n");
    } else {
      resp.status(http::StatusCodeServiceUnavailable);
      resp.body("Starting\n");
    }
    return resp;
  });
}

void HttpServer::createEncoders() {
#ifdef AERONET_ENABLE_ZLIB
  _encoders[static_cast<std::size_t>(Encoding::gzip)] =
      std::make_unique<ZlibEncoder>(details::ZStreamRAII::Variant::gzip, _config.compression);
  _encoders[static_cast<std::size_t>(Encoding::deflate)] =
      std::make_unique<ZlibEncoder>(details::ZStreamRAII::Variant::deflate, _config.compression);
#endif
#ifdef AERONET_ENABLE_ZSTD
  _encoders[static_cast<std::size_t>(Encoding::zstd)] = std::make_unique<ZstdEncoder>(_config.compression);
#endif
#ifdef AERONET_ENABLE_BROTLI
  _encoders[static_cast<std::size_t>(Encoding::br)] = std::make_unique<BrotliEncoder>(_config.compression);
#endif
}

}  // namespace aeronet