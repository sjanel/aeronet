#include <cassert>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-lifecycle-tracker.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timer-fd.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tracing/tracer.hpp"

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

  LifecycleTrackerGuard(const LifecycleTrackerGuard&) = delete;
  LifecycleTrackerGuard& operator=(const LifecycleTrackerGuard&) = delete;
  LifecycleTrackerGuard(LifecycleTrackerGuard&&) = delete;
  LifecycleTrackerGuard& operator=(LifecycleTrackerGuard&&) = delete;

  ~LifecycleTrackerGuard() {
    if (auto locked = _tracker.lock()) {
      locked->notifyServerStopped();
    }
  }

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
      _compressionState(_config.compression),
      _listenSocket(Socket::Type::StreamNonBlock),
      _eventLoop(_config.pollInterval),
      _router(std::move(routerConfig)),
      _telemetry(_config.telemetry) {
  initListener();
}

SingleHttpServer::SingleHttpServer(HttpServerConfig cfg, Router router)
    : _config(std::move(cfg)),
      _compressionState(_config.compression),
      _listenSocket(Socket::Type::StreamNonBlock),
      _eventLoop(_config.pollInterval),
      _router(std::move(router)),
      _telemetry(_config.telemetry) {
  initListener();
}

SingleHttpServer::SingleHttpServer(const SingleHttpServer& other, NativeHandle sharedListenFd)
    : _callbacks([&other] {
        // Must validate *before* moving any members.
        // Otherwise we can move out (and destroy) connection storage while the event-loop
        // thread is still running against the source object, causing UAF.
        if (!other._lifecycle.isIdle()) {
          throw std::logic_error("Cannot copy-construct from a running SingleHttpServer");
        }
        return other._callbacks;
      }()),
      _updates(other._updates),
      _config(other._config),
      _compressionState(_config.compression),
      // do not copy the decompression state, we just use our own.
      // do not initialize listenSocket and eventLoop, they will be initialized in initListener.
      _router(other._router),
      _telemetry(_config.telemetry) {
#ifdef AERONET_ENABLE_OPENSSL
  // Copy-constructor inherits the shared ticket key store for MultiHttpServer fan-out.
  _tls.sharedTicketKeyStore = other._tls.sharedTicketKeyStore;
#endif

  initListener(sharedListenFd);
}

SingleHttpServer& SingleHttpServer::operator=(const SingleHttpServer& other) {
  if (this != &other) {
    if (!other._lifecycle.isIdle()) {
      throw std::logic_error("Cannot copy-assign from a running SingleHttpServer");
    }

    stop();

    auto lifecycleTracker = _lifecycleTracker;

    *this = SingleHttpServer(other);

    _lifecycleTracker = std::move(lifecycleTracker);
  }
  return *this;
}

// NOLINTNEXTLINE(bugprone-exception-escape,performance-noexcept-move-constructor)
SingleHttpServer::SingleHttpServer(SingleHttpServer&& other)
    : _stats([&other] {
        // Must validate *before* moving any members.
        // Otherwise we can move out (and destroy) connection storage while the event-loop
        // thread is still running against the source object, causing UAF.
        if (!other._lifecycle.isIdle()) {
          throw std::logic_error("Cannot move-construct a running SingleHttpServer");
        }
        return std::exchange(other._stats, {});
      }()),
      _callbacks(std::move(other._callbacks)),
      _updates(std::move(other._updates)),
      _config(std::move(other._config)),
      _compressionState(std::move(other._compressionState)),
      _decompressionState(std::move(other._decompressionState)),
      _listenSocket(std::move(other._listenSocket)),
      _maintenanceTimer(std::move(other._maintenanceTimer)),
      _eventLoop(std::move(other._eventLoop)),
      _lifecycle(std::move(other._lifecycle)),
#ifndef AERONET_LINUX
      _lastMaintenanceTp(other._lastMaintenanceTp),
#endif
      _router(std::move(other._router)),
      _connections(std::move(other._connections)),
      _sharedBuffers(std::move(other._sharedBuffers)),
      _telemetry(std::move(other._telemetry)),
      _internalHandle(std::move(other._internalHandle)),
      _lifecycleTracker(std::move(other._lifecycleTracker))
#ifdef AERONET_ENABLE_OPENSSL
      ,
      _tls(std::move(other._tls))
#endif
{
  _compressionState.pCompressionConfig = &_config.compression;
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
    _callbacks = std::move(other._callbacks);
    _updates = std::move(other._updates);
    _config = std::move(other._config);

    _compressionState = std::move(other._compressionState);
    _compressionState.pCompressionConfig = &_config.compression;

    _decompressionState = std::move(other._decompressionState);
    _listenSocket = std::move(other._listenSocket);
    _maintenanceTimer = std::move(other._maintenanceTimer);
    _eventLoop = std::move(other._eventLoop);
    _lifecycle = std::move(other._lifecycle);
#ifndef AERONET_LINUX
    _lastMaintenanceTp = other._lastMaintenanceTp;
#endif
    _router = std::move(other._router);
    _connections = std::move(other._connections);
    _sharedBuffers = std::move(other._sharedBuffers);
    _telemetry = std::move(other._telemetry);
    _internalHandle = std::move(other._internalHandle);
    _lifecycleTracker = std::move(other._lifecycleTracker);
#ifdef AERONET_ENABLE_OPENSSL
    _tls = std::move(other._tls);
#endif

    other._lifecycle.reset();
  }
  return *this;
}

// Performs full listener initialization (RAII style) so that port() is valid immediately after construction.
// Steps (in order) and rationale / failure characteristics:
//   1. socket(AF_INET, SOCK_STREAM, 0)
//        - Expected to succeed under normal conditions. Failure indicates resource exhaustion
//          (error::kTooManyFiles per-process fd limit, ENFILE system-wide, error::kNoBufferSpace/ENOMEM) or
//          misconfiguration (rare EACCES).
//   2. setsockopt(SO_REUSEADDR)
//        - Practically infallible unless programming error (EINVAL) or extreme memory pressure (ENOMEM).
//          Mandatory to allow rapid restart after TIME_WAIT collisions.
//   3. setsockopt(SO_REUSEPORT) (optional best-effort)
//        - Enabled only if cfg.reusePort. May fail on older kernels (EOPNOTSUPP/EINVAL) -> logged as warning
//        only,
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
void SingleHttpServer::initListener(NativeHandle listenFd) {
  if (_config.nbThreads > 1U) {
    throw std::invalid_argument("SingleHttpServer cannot be configured with nbThreads > 1");
  }
  _config.validate();

  if (!_listenSocket) {
#ifdef AERONET_MACOS
    if (listenFd != kInvalidHandle) {
      _listenSocket = Socket(BaseFd::Borrow(listenFd));
      _ownsListenSocket = false;
    } else {
      _ownsListenSocket = true;
#endif
      _listenSocket = Socket(Socket::Type::StreamNonBlock);
#ifdef AERONET_MACOS
    }
#endif
    _eventLoop = EventLoop(_config.pollInterval);
  }

#ifdef AERONET_ENABLE_OPENSSL
  // Initialize TLS context if requested (OpenSSL build).
  if (_config.tls.enabled) {
    _tls.ctxHolder = std::make_shared<TlsContext>(_config.tls, _tls.sharedTicketKeyStore);
  }
#endif

#ifdef AERONET_MACOS
  if (listenFd == kInvalidHandle) {
#endif
    _listenSocket.bindAndListen(_config.reusePort, _config.port);
    listenFd = _listenSocket.fd();
#ifdef AERONET_MACOS
  }
#endif

  _eventLoop.addOrThrow(EventLoop::EventFd{listenFd, EventIn});
  _eventLoop.addOrThrow(EventLoop::EventFd{_lifecycle.wakeupFd.fd(), EventIn});

  updateMaintenanceTimer();
  _eventLoop.addOrThrow(EventLoop::EventFd{_maintenanceTimer.fd(), EventIn});
}

void SingleHttpServer::prepareRun() {
  if (_lifecycle.isActive()) {
    throw std::logic_error("Server is already running");
  }
  if (!_listenSocket) {
    initListener();
  }
  if (!isInMultiHttpServer()) {
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
  // Check the predicate before initializing resources.  When used inside
  // MultiHttpServer, the stop flag is set before server.stop() calls
  // closeListener().  Without this early check, a late-scheduled thread
  // could be inside initListener() (creating socket / event loop) while
  // the main thread concurrently calls closeListener(), causing a data race.
  if (predicate()) {
    return;
  }
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
            } catch (const std::exception& ex) {
              log::error("Event loop thread exiting due to exception: {}", ex.what());
              *errorPtr = std::current_exception();
            } catch (...) {
              log::error("Event loop thread exiting due to unknown exception");
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
            } catch (const std::exception& ex) {
              log::error("Event loop thread exiting due to exception: {}", ex.what());
              *errorPtr = std::current_exception();
            } catch (...) {
              log::error("Event loop thread exiting due to unknown exception");
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
            } catch (const std::exception& ex) {
              log::error("Event loop thread exiting due to exception: {}", ex.what());
              *errorPtr = std::current_exception();
            } catch (...) {
              log::error("Event loop thread exiting due to unknown exception");
              *errorPtr = std::current_exception();
            }
          }),
          std::move(errorPtr)};
}

void SingleHttpServer::stop() noexcept {
  auto prevState = _lifecycle.exchangeStopping();
  if (prevState == internal::Lifecycle::State::Running || prevState == internal::Lifecycle::State::Draining) {
    // Wake the event loop immediately so it notices the Stopping state and exits
    // before we close the listen socket.  On Windows, closing the listen socket
    // while WSAPoll holds it can cause WSAPoll to hang indefinitely.
    _lifecycle.wakeupFd.send();

    // Stop internal handle if start() was used (non-blocking API).
    // This joins the background thread, after which the thread has already called _lifecycle.reset().
    _internalHandle.stop();

    // In multi-server mode the background thread is NOT owned by _internalHandle — it is managed
    // by MultiHttpServer::AsyncHandle and will be joined later.  The event-loop thread closes
    // the listener itself when it processes the Stopping state, so we must NOT call
    // closeListener() here — doing so would close the socket while WSAPoll still holds it,
    // causing undefined behavior on Windows.
    if (!isInMultiHttpServer()) {
      // Close the listener AFTER the event-loop thread has exited (joined via _internalHandle),
      // so WSAPoll never sees the invalidated listen socket fd.
      closeListener();
      _lifecycle.reset();
    }
  } else if (prevState != internal::Lifecycle::State::Stopping) {
    // Idle — still ensure the listener is closed.
    // When Stopping, another stop() call (or the event-loop thread) is already
    // handling shutdown.  Calling closeListener() here would race with
    // the event-loop thread's WSAPoll on Windows.
    closeListener();
  }
}

void SingleHttpServer::beginDrain(std::chrono::milliseconds maxWait) noexcept {
  if (!_lifecycle.isActive() || _lifecycle.isStopping()) {
    return;
  }

  const bool hasDeadline = maxWait.count() > 0;
  const auto deadline = hasDeadline ? _connections.now + maxWait : std::chrono::steady_clock::time_point{};

  if (_lifecycle.isDraining()) {
    if (hasDeadline) {
      _lifecycle.shrinkDeadline(deadline);
    }
    return;
  }

  const auto nbActiveConnections = _connections.size();
  if (nbActiveConnections != 0) {
    log::info("Initiating graceful drain with {} active connection(s)", nbActiveConnections);
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
    const bool isReady = _lifecycle.ready();
    return HttpResponse(isReady ? http::StatusCodeOK : http::StatusCodeServiceUnavailable,
                        isReady ? "OK\n" : "Not Ready\n");
  });

  // Startup: Returns 200 once server is running (listener active).
  // Note: Since aeronet has no separate initialization phase, this is essentially
  // equivalent to liveness. Provided for Kubernetes compatibility where startup
  // probes can have different timeout/period settings for slow-starting applications.
  _router.setPath(http::Method::GET, _config.builtinProbes.startupPath(), [this](const HttpRequest&) {
    const bool isStarted = _lifecycle.started();
    return HttpResponse(isStarted ? http::StatusCodeOK : http::StatusCodeServiceUnavailable,
                        isStarted ? "OK\n" : "Starting\n");
  });
}

}  // namespace aeronet