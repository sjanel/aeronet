#include <atomic>
#include <cassert>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/lifecycle.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log-noexcept.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-lifecycle-tracker.hpp"
#ifdef AERONET_LINUX
#include "aeronet/sigpipe-blocker.hpp"
#endif
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/timer-fd.hpp"
#include "aeronet/zerocopy-mode.hpp"
#include "aeronet/zerocopy.hpp"

#ifdef AERONET_ENABLE_GLAZE
#include <filesystem>
#include <string>

#include "aeronet/config-file.hpp"
#include "aeronet/server-config-loader.hpp"
#endif

namespace aeronet {

namespace {

class LifecycleTrackerGuard {
 public:
  explicit LifecycleTrackerGuard(std::weak_ptr<ServerLifecycleTracker> tracker) : _tracker(std::move(tracker)) {
    if (auto locked = _tracker.lock()) {
      locked->notifyServerLaunched();
    }
  }

  LifecycleTrackerGuard(const LifecycleTrackerGuard&) = delete;
  LifecycleTrackerGuard& operator=(const LifecycleTrackerGuard&) = delete;
  // Movable to be handed over to the worker thread: the moved-from guard (empty tracker) notifies nothing.
  LifecycleTrackerGuard(LifecycleTrackerGuard&&) noexcept = default;
  LifecycleTrackerGuard& operator=(LifecycleTrackerGuard&&) = delete;

  ~LifecycleTrackerGuard() {
    if (auto locked = _tracker.lock()) {
      locked->notifyServerStopped();
    }
  }

 private:
  std::weak_ptr<ServerLifecycleTracker> _tracker;
};

// Publishes the calling thread as the event-loop thread for the duration of runUntilStarted().
class EventLoopThreadRAII {
 public:
  explicit EventLoopThreadRAII(internal::Lifecycle& lifecycle) : _lifecycle(lifecycle) {
    _lifecycle.eventLoopThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
  }

  EventLoopThreadRAII(const EventLoopThreadRAII&) = delete;
  EventLoopThreadRAII(EventLoopThreadRAII&&) noexcept = delete;
  EventLoopThreadRAII& operator=(const EventLoopThreadRAII&) = delete;
  EventLoopThreadRAII& operator=(EventLoopThreadRAII&&) noexcept = delete;

  ~EventLoopThreadRAII() { _lifecycle.eventLoopThread.store(std::thread::id{}, std::memory_order_relaxed); }

 private:
  internal::Lifecycle& _lifecycle;
};

class LifecycleResetterRAII {
 public:
  explicit LifecycleResetterRAII(internal::Lifecycle& lifecycle) : _lifecycle(lifecycle) {}

  LifecycleResetterRAII(const LifecycleResetterRAII&) = delete;
  LifecycleResetterRAII(LifecycleResetterRAII&&) noexcept = delete;
  LifecycleResetterRAII& operator=(const LifecycleResetterRAII&) = delete;
  LifecycleResetterRAII& operator=(LifecycleResetterRAII&&) noexcept = delete;

  ~LifecycleResetterRAII() { _lifecycle.reset(); }

 private:
  internal::Lifecycle& _lifecycle;
};

}  // namespace

SingleHttpServer::AsyncHandle::AsyncHandle(std::jthread thread, std::shared_ptr<std::exception_ptr> error)
    : _thread(std::move(thread)), _error(std::move(error)) {}

void SingleHttpServer::AsyncHandle::stop() noexcept {
  // Not from the event-loop thread itself (e.g. SingleHttpServer::stop() called from a handler): it cannot join itself,
  // and stops on its own once the current callback returns. It is then joined by a later stop() or the destructor.
  if (_thread.joinable() && _thread.get_id() != std::this_thread::get_id()) {
    _thread.request_stop();
    _thread.join();
  }
}

void SingleHttpServer::AsyncHandle::rethrowIfError() {
  if (_error && *_error) {
    std::rethrow_exception(*_error);
  }
}

// The default globalHeaders ("server: aeronet") never contain the separator that makes ConcatenatedHeaders throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
SingleHttpServer::SingleHttpServer() noexcept = default;

SingleHttpServer::SingleHttpServer(HttpServerConfig config, RouterConfig routerConfig)
    : _ctx(std::move(config), Router(std::move(routerConfig))), _listenSocket(Socket::Type::StreamNonBlock) {
  initListener();
}

SingleHttpServer::SingleHttpServer(HttpServerConfig cfg, Router router)
    : _ctx(std::move(cfg), std::move(router)), _listenSocket(Socket::Type::StreamNonBlock) {
  initListener();
}

#ifdef AERONET_ENABLE_GLAZE
SingleHttpServer::SingleHttpServer(const std::filesystem::path& configPath) {
  auto config = detail::ParseConfigFile(configPath);
  *this = SingleHttpServer(std::move(config.server), std::move(config.router));
}

SingleHttpServer::SingleHttpServer(const std::filesystem::path& configPath, Router router) {
  auto config = detail::ParseConfigFile(configPath);
  *this = SingleHttpServer(std::move(config.server), std::move(router));
}

std::string SingleHttpServer::dumpConfig(ConfigFormat format) const {
  TopLevelConfig config{.server = _ctx.config, .router = _ctx.router.config()};
  return detail::SerializeConfig(config, format);
}

void SingleHttpServer::saveConfig(const std::filesystem::path& filePath) const {
  detail::WriteConfigFile(filePath, dumpConfig(detail::DetectFormat(filePath)));
}
#endif

const internal::ServerContext& SingleHttpServer::CopyableContext(const SingleHttpServer& other) {
  // Must validate *before* copying any members: the event-loop thread may still be running against the source object.
  if (!other._ctx.lifecycle.isIdle()) {
    throw std::logic_error("Cannot copy-construct from a running SingleHttpServer");
  }
  return other._ctx;
}

internal::ServerContext&& SingleHttpServer::MovableContext(SingleHttpServer& other) {
  // Must validate *before* moving any members.
  // Otherwise we can move out (and destroy) connection storage while the event-loop
  // thread is still running against the source object, causing UAF.
  if (!other._ctx.lifecycle.isIdle()) {
    throw std::logic_error("Cannot move-construct a running SingleHttpServer");
  }
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Deferred work still running for its closed connections posts its completion to 'other'.
  other.waitForOrphanedAsyncWork();
#endif
  return std::move(other._ctx);
}

SingleHttpServer::SingleHttpServer(const SingleHttpServer& other, NativeHandle sharedListenFd)
    // The listen socket and the event loop are initialized by initListener().
    : _ctx(CopyableContext(other), internal::ServerContext::CopySetupTag{}),
      _callbacksUpdates(other._callbacksUpdates),
      _hasCallbacksUpdates(other._hasCallbacksUpdates.load(std::memory_order_relaxed)) {
  initListener(sharedListenFd);
}

SingleHttpServer::~SingleHttpServer() {
  stop();
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  waitForOrphanedAsyncWork();
#endif
#ifdef AERONET_LINUX
  _ctx.io.waitForResetZerocopyBuffers();
#endif
}

SingleHttpServer& SingleHttpServer::operator=(const SingleHttpServer& other) {
  if (this != &other) {
    if (!other._ctx.lifecycle.isIdle()) {
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
    : _ctx(MovableContext(other)),
      _callbacksUpdates(std::move(other._callbacksUpdates)),
      _hasCallbacksUpdates(other._hasCallbacksUpdates.exchange(false, std::memory_order_relaxed)),
      _listenSocket(std::move(other._listenSocket)),
#ifdef AERONET_LINUX
      _portReservation(std::move(other._portReservation)),
#endif
      _maintenanceTimer(std::move(other._maintenanceTimer)),
#ifndef AERONET_LINUX
      _lastMaintenanceTp(other._lastMaintenanceTp),
#endif
      _internalHandle(std::move(other._internalHandle)),
      _lifecycleTracker(std::move(other._lifecycleTracker))
#ifdef AERONET_MACOS
      ,
      _ownsListenSocket(std::exchange(other._ownsListenSocket, false))
#endif
{
  other._ctx.lifecycle.reset();
}

// NOLINTNEXTLINE(bugprone-exception-escape,performance-noexcept-move-constructor)
SingleHttpServer& SingleHttpServer::operator=(SingleHttpServer&& other) {
  if (this != &other) {
    stop();

    if (!other._ctx.lifecycle.isIdle()) {
      other.stop();
      throw std::logic_error("Cannot move-assign from a running SingleHttpServer");
    }
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    // Deferred work still running for closed connections posts its completion to the server that ran it.
    waitForOrphanedAsyncWork();
    other.waitForOrphanedAsyncWork();
#endif
#ifdef AERONET_LINUX
    _ctx.io.waitForResetZerocopyBuffers();
#endif
    _ctx = std::move(other._ctx);
    _callbacksUpdates = std::move(other._callbacksUpdates);
    _hasCallbacksUpdates.store(other._hasCallbacksUpdates.exchange(false, std::memory_order_relaxed),
                               std::memory_order_relaxed);
    _listenSocket = std::move(other._listenSocket);
#ifdef AERONET_LINUX
    _portReservation = std::move(other._portReservation);
#endif
    _maintenanceTimer = std::move(other._maintenanceTimer);
#ifndef AERONET_LINUX
    _lastMaintenanceTp = other._lastMaintenanceTp;
#endif
    _internalHandle = std::move(other._internalHandle);
    _lifecycleTracker = std::move(other._lifecycleTracker);
#ifdef AERONET_MACOS
    _ownsListenSocket = std::exchange(other._ownsListenSocket, false);
#endif
    other._ctx.lifecycle.reset();
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
  if (_ctx.config.nbThreads > 1U) {
    throw std::invalid_argument("SingleHttpServer cannot be configured with nbThreads > 1");
  }
  _ctx.config.validate();

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
    _ctx.eventLoop = EventLoop(internal::MakePollTimeoutPolicy(_ctx.config));
  }

#ifdef AERONET_ENABLE_OPENSSL
  // Initialize TLS context if requested (OpenSSL build).
  _ctx.tls.initContext();
#endif

#ifdef AERONET_MACOS
  if (listenFd == kInvalidHandle) {
#endif
#ifdef AERONET_LINUX
    const bool ephemeralPort = _ctx.config.port == 0;
    // The accepted sockets inherit SO_ZEROCOPY: set it on the listener before it listens (a socket accepted without it
    // would silently copy the MSG_ZEROCOPY sends, without completion notification), whatever the zerocopy mode, so that
    // a configuration update can enable zerocopy for the next connections.
    _ctx.io.setZerocopySupported(EnableZeroCopy(_listenSocket.fd()) == ZeroCopyEnableResult::Enabled);
    if (!_ctx.io.zerocopySupported() && _ctx.config.zerocopyMode == ZerocopyMode::Enabled) {
      log::warn("MSG_ZEROCOPY is not supported (SO_ZEROCOPY failed): the responses are copied");
    }
#endif
    _listenSocket.bindAndListen(_ctx.config.reusePort, _ctx.config.port);
    listenFd = _listenSocket.fd();
#ifdef AERONET_LINUX
    // The resolved ephemeral port is kept across restarts (rebound by the next initListener()), but the listener is
    // closed while stopped: another process could then obtain the port with bind(0), and with SO_REUSEPORT the restart
    // would silently share the incoming connections with it. Reserve it with a bound socket that never listens:
    // bind(0) never returns a port in use, and a socket that does not listen receives no connection.
    // Linux only: on other platforms, a bound socket that does not listen may be picked for incoming connections.
    if (ephemeralPort && _ctx.config.reusePort) {
      Socket reservation(Socket::Type::StreamNonBlock);
      if (reservation.tryBind(_ctx.config.reusePort, _ctx.config.port)) {
        _portReservation = std::move(reservation);
      }
    }
#endif
#ifdef AERONET_MACOS
  }
#endif

  _ctx.eventLoop.addOrThrow(EventLoop::EventFd{listenFd, EventIn});
  _ctx.eventLoop.addOrThrow(EventLoop::EventFd{_ctx.lifecycle.wakeupFd.fd(), EventIn});

  updateMaintenanceTimer();
  _ctx.eventLoop.addOrThrow(EventLoop::EventFd{_maintenanceTimer.fd(), EventIn});
}

void SingleHttpServer::prepareRun() {
  assert(_ctx.lifecycle.isStarting());
  if (!_listenSocket) {
    initListener();
  }
  if (!isInMultiHttpServer()) {
    // In MultiHttpServer, logging is done at that level instead.
    log::info("Server running on port :{}", port());
  }

  // Register builtin probes handlers if enabled in config
  registerBuiltInProbes();

  // Initialize access log writer if configured
  _ctx.accessLog = AccessLogWriter(_ctx.config.accessLog);

  // Pre-clamp per-route limits against the global server limits so the hot path only needs a single comparison (no
  // runtime std::min).
  _ctx.router.clampConfigs(_ctx.config.maxHeaderBytes, _ctx.config.maxBodyBytes);

  _ctx.lifecycle.enterRunning();
}

void SingleHttpServer::beginStartup() {
  std::scoped_lock lock(_ctx.updates.lock);
  _ctx.lifecycle.enterStarting();
}

void SingleHttpServer::run() {
  beginStartup();
  runUntilStarted([] { return false; });
}

void SingleHttpServer::runUntilStarted(const std::function<bool()>& predicate) {
#ifdef AERONET_LINUX
  // sendfile() and OpenSSL's socket writes raise SIGPIPE on connections reset by the peer: keep it away from the
  // process for the whole lifetime of the event loop, connection teardown included (declared first, destroyed last).
  const SigpipeBlocker sigpipeBlocker;
#endif

  EventLoopThreadRAII eventLoopThread(_ctx.lifecycle);
  LifecycleResetterRAII resetter(_ctx.lifecycle);

  prepareRun();

  while (_ctx.lifecycle.isActive() && !predicate()) {
    eventLoop();
  }

  // A predicate can become true after eventLoop() returns, bypassing the teardown normally performed inside it.
  // Close all network resources before leaving runUntil() on every platform. This must remain on the event-loop
  // thread because Windows WSAPoll registrations cannot be mutated safely by the controller thread.
  if (_ctx.lifecycle.isActive()) {
    closeListenerAndConnections();
  }
}

void SingleHttpServer::start() { _internalHandle = startDetached(); }

void SingleHttpServer::runUntil(const std::function<bool()>& predicate) {
  if (!predicate()) {
    beginStartup();
    runUntilStarted(predicate);
  }
}

SingleHttpServer::AsyncHandle SingleHttpServer::launchDetached(std::function<bool()> extraPredicate) {
  auto errorPtr = std::make_shared<std::exception_ptr>();

  beginStartup();
  try {
    // A MultiHttpServer worker is counted from its launch (on the controller thread) until its thread is done, not only
    // while its event loop runs: MultiHttpServer::run() then waits for exactly the launched workers, including the ones
    // that stop - or fail to start - before it gets to wait for them.
    LifecycleTrackerGuard trackerGuard(_lifecycleTracker);

    // Construct the thread before moving errorPtr into AsyncHandle. The evaluations of the constructor arguments are
    // indeterminately sequenced: MSVC may move the shared_ptr first, leaving the worker's lambda with a null errorPtr
    // when its predicate throws.
    std::jthread thread([this, pred = std::move(extraPredicate), errorPtr,
                         trackerGuard = std::move(trackerGuard)](const std::stop_token& st) mutable {
      // Notifies the end of this worker when leaving this scope, after runUntilStarted() has fully completed.
      const LifecycleTrackerGuard exitGuard(std::move(trackerGuard));

      const auto captureError = [&errorPtr] {
        if (!*errorPtr) {
          *errorPtr = std::current_exception();
        }
      };

      // A throwing predicate is treated as a normal stop request instead of letting the exception unwind
      // across this thread's runUntilStarted() RAII guard (LifecycleResetterRAII):
      // captured here, at the call site, and surfaced later via rethrowIfError().
      auto safePredicate = [&st, &pred, &captureError] -> bool {
        if (st.stop_requested()) {
          return true;
        }
        if (!pred) {
          return false;
        }
        try {
          return pred();
        } catch (const std::exception& ex) {
          log::error("Worker predicate exiting due to exception: {}", ex.what());
          captureError();
          return true;
        } catch (...) {
          log::error("Worker predicate exiting due to unknown exception");
          captureError();
          return true;
        }
      };
      try {
        runUntilStarted(safePredicate);
      } catch (const std::exception& ex) {
        log::error("Event loop thread exiting due to exception: {}", ex.what());
        captureError();
      } catch (...) {
        log::error("Event loop thread exiting due to unknown exception");
        captureError();
      }
    });

    return {std::move(thread), std::move(errorPtr)};
  } catch (...) {
    // No thread will ever run prepareRun()/reset() to move us out of Starting - do it here so a concurrent stop()
    // (which now blocks on Starting) doesn't wait forever, then rethrow.
    _ctx.lifecycle.reset();
    throw;
  }
}

SingleHttpServer::AsyncHandle SingleHttpServer::startDetachedAndStopWhen(std::function<bool()> predicate) {
  return launchDetached(std::move(predicate));
}

SingleHttpServer::AsyncHandle SingleHttpServer::startDetachedWithStopToken(std::stop_token token) {
  return launchDetached([token = std::move(token)] { return token.stop_requested(); });
}

void SingleHttpServer::stop() noexcept {
  const auto prevState = _ctx.lifecycle.exchangeStopping();
  assert(prevState != internal::Lifecycle::State::Starting);
  if (prevState == internal::Lifecycle::State::Running || prevState == internal::Lifecycle::State::Draining) {
    // Wake the event loop immediately so it notices the Stopping state: the event-loop thread then closes the listener
    // and the connections and resets the lifecycle itself. None of them may be touched from here while it may still run
    // (it polls the listen socket, and a reset lifecycle would allow a restart while it is still running; on Windows,
    // closing the listen socket while WSAPoll holds it can even hang).
    _ctx.lifecycle.wakeupFd.send();

    // Joins the event-loop thread if start() was used (non-blocking API). Otherwise (run() / runUntil() from another
    // thread, startDetached*(), or a MultiHttpServer worker), the thread running it is joined by its owner.
    _internalHandle.stop();
  } else if (prevState != internal::Lifecycle::State::Stopping) {
    // Idle - still ensure the listener is closed.
    // When Stopping, another stop() call (or the event-loop thread) is already
    // handling shutdown.  Calling closeListener() here would race with
    // the event-loop thread's WSAPoll on Windows.
    // The event loop may also have stopped by itself (drain completed, stop() from a handler): join its thread if
    // start() was used.
    _internalHandle.stop();
    closeListener();
  }
}

void SingleHttpServer::beginDrain(std::chrono::milliseconds maxWait) noexcept {
  // Like stop(), wait for an in-flight startup instead of dropping the request: otherwise a drain requested right after
  // start() (e.g. MultiHttpServer::beginDrain() while some workers are still in prepareRun()) would be lost, and these
  // servers would then serve normal traffic and report ready.
  const auto current = _ctx.lifecycle.waitWhileStarting();
  if (current == internal::Lifecycle::State::Idle || current == internal::Lifecycle::State::Stopping) {
    return;
  }

  if (maxWait.count() > 0) {
    // Set before entering Draining, so that the event loop never observes Draining without it. If the server is already
    // draining (possibly from a concurrent call), keeps the earliest of both deadlines.
    // Fresh clock read rather than the event loop's cached connections.now: it is written by the event-loop thread,
    // and is stale (or still default-initialized) until the first poll() of a just-started server returns.
    _ctx.lifecycle.shrinkDeadline(std::chrono::steady_clock::now() + maxWait);
  }

  // Only from Running: a concurrent stop() (Stopping) must not be turned back into a drain.
  // The connections are owned by the event loop: their count cannot be read from here.
  if (current == internal::Lifecycle::State::Running && _ctx.lifecycle.enterDraining()) {
    log_noexcept::info("Initiating graceful drain");
  }
  // Keep listener open during drain to allow health probes to connect and receive 503 status.
  // Regular connections will still be accepted but will receive Connection: close headers.
}

void SingleHttpServer::registerBuiltInProbes() {
  if (!_ctx.config.builtinProbes.enabled) {
    return;
  }
  // True when this server is a MultiHttpServer worker whose probes are served by a dedicated probe listener.
  // Such workers skip registering the inline probe routes (they live on the dedicated port); the probe listener reads
  // their loop heartbeat (see internal::Lifecycle::loopHeartbeat, published by every event loop) to detect a wedge.
  // A standalone SingleHttpServer ignores dedicatedPort: it has no worker pool to isolate probes from.
  if (_ctx.config.builtinProbes.dedicatedPort != 0 && isInMultiHttpServer()) {
    // Probes are answered by MultiHttpServer's dedicated probe listener on its own port/thread; registering them
    // inline here too would just re-expose them on the (contended) application port, defeating the isolation.
    return;
  }

  // Liveness: Always returns 200 OK if the server is responding.
  // Indicates the application is alive (not deadlocked/crashed).
  _ctx.router.setPath(http::Method::GET, _ctx.config.builtinProbes.livenessPath(),
                      [](const HttpRequestView& req) { return req.makeResponse("OK"); });

  // Readiness: Returns 200 when ready to serve traffic, 503 during drain.
  // Used by load balancers to determine if instance should receive traffic.
  _ctx.router.setPath(http::Method::GET, _ctx.config.builtinProbes.readinessPath(), [this](const HttpRequestView& req) {
    const bool isReady = _ctx.lifecycle.ready();
    return req.makeResponse(isReady ? http::StatusCodeOK : http::StatusCodeServiceUnavailable,
                            isReady ? "OK" : "Not Ready");
  });

  // Startup: Returns 200 once server is running (listener active).
  // Note: Since aeronet has no separate initialization phase, this is essentially
  // equivalent to liveness. Provided for Kubernetes compatibility where startup
  // probes can have different timeout/period settings for slow-starting applications.
  _ctx.router.setPath(http::Method::GET, _ctx.config.builtinProbes.startupPath(), [this](const HttpRequestView& req) {
    const bool isStarted = _ctx.lifecycle.started();
    return req.makeResponse(isStarted ? http::StatusCodeOK : http::StatusCodeServiceUnavailable,
                            isStarted ? "OK" : "Starting");
  });
}

}  // namespace aeronet
