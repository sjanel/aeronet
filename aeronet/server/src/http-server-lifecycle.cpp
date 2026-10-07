#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/lifecycle.hpp"
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
#include "aeronet/tls-config.hpp"
#include "aeronet/tracing/tracer.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-context.hpp"
#endif

#ifdef AERONET_ENABLE_GLAZE
#include <filesystem>
#include <ios>
#include <string>

#include "aeronet/aeronet-config.hpp"
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

PollTimeoutPolicy SingleHttpServer::MakePollTimeoutPolicy(const HttpServerConfig& config) {
  return PollTimeoutPolicy{
      .baseTimeout = config.pollInterval,
      .minFactor = config.pollIntervalMinFactor,
      .maxFactor = config.pollIntervalMaxFactor,
  };
}

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

SingleHttpServer::SingleHttpServer(HttpServerConfig config, RouterConfig routerConfig)
    : _config(std::move(config)),
      _compressionState(_config.compression),
      _listenSocket(Socket::Type::StreamNonBlock),
      _eventLoop(MakePollTimeoutPolicy(_config)),
      _router(std::move(routerConfig)),
      _telemetry(_config.telemetry) {
  initListener();
}

SingleHttpServer::SingleHttpServer(HttpServerConfig cfg, Router router)
    : _config(std::move(cfg)),
      _compressionState(_config.compression),
      _listenSocket(Socket::Type::StreamNonBlock),
      _eventLoop(MakePollTimeoutPolicy(_config)),
      _router(std::move(router)),
      _telemetry(_config.telemetry) {
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
  TopLevelConfig config{.server = _config, .router = _router.config()};
  return detail::SerializeConfig(config, format);
}

void SingleHttpServer::saveConfig(const std::filesystem::path& filePath) const {
  auto format = detail::DetectFormat(filePath);
  auto content = dumpConfig(format);
  std::ofstream ofs(filePath, std::ios::binary);
  if (!ofs) {
    throw std::runtime_error("Failed to open file for writing: " + filePath.string());
  }
  ofs << content;
}
#endif

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
      _callbacksUpdates(other._callbacksUpdates),
      _hasCallbacksUpdates(other._hasCallbacksUpdates.load(std::memory_order_relaxed)),
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

SingleHttpServer::~SingleHttpServer() {
  stop();
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  waitForOrphanedAsyncWork();
#endif
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
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
        // Deferred work still running for its closed connections posts its completion to 'other'.
        other.waitForOrphanedAsyncWork();
#endif
        return std::exchange(other._stats, {});
      }()),
      _callbacks(std::move(other._callbacks)),
      _updates(std::move(other._updates)),
      _callbacksUpdates(std::move(other._callbacksUpdates)),
      _hasCallbacksUpdates(other._hasCallbacksUpdates.exchange(false, std::memory_order_relaxed)),
      _config(std::move(other._config)),
      _compressionState(std::move(other._compressionState)),
      _decompressionState(std::move(other._decompressionState)),
      _listenSocket(std::move(other._listenSocket)),
      _portReservation(std::move(other._portReservation)),
      _maintenanceTimer(std::move(other._maintenanceTimer)),
      _eventLoop(std::move(other._eventLoop)),
      _lifecycle(std::move(other._lifecycle)),
#ifndef AERONET_LINUX
      _lastMaintenanceTp(other._lastMaintenanceTp),
#endif
      _router(std::move(other._router)),
      _connections(std::move(other._connections)),
      _keepAliveDeadlines(std::move(other._keepAliveDeadlines)),
      _sharedBuffers(std::move(other._sharedBuffers)),
      _telemetry(std::move(other._telemetry)),
      _internalHandle(std::move(other._internalHandle)),
      _lifecycleTracker(std::move(other._lifecycleTracker)),
      _pendingReadFds(std::move(other._pendingReadFds)),
      _connectionSweepState(std::exchange(other._connectionSweepState, {}))
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
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    // Deferred work still running for closed connections posts its completion to the server that ran it.
    waitForOrphanedAsyncWork();
    other.waitForOrphanedAsyncWork();
#endif
    _stats = std::exchange(other._stats, {});
    _callbacks = std::move(other._callbacks);
    _updates = std::move(other._updates);
    _callbacksUpdates = std::move(other._callbacksUpdates);
    _hasCallbacksUpdates.store(other._hasCallbacksUpdates.exchange(false, std::memory_order_relaxed),
                               std::memory_order_relaxed);
    _config = std::move(other._config);

    _compressionState = std::move(other._compressionState);
    _compressionState.pCompressionConfig = &_config.compression;

    _decompressionState = std::move(other._decompressionState);
    _listenSocket = std::move(other._listenSocket);
    _portReservation = std::move(other._portReservation);
    _maintenanceTimer = std::move(other._maintenanceTimer);
    _eventLoop = std::move(other._eventLoop);
    _lifecycle = std::move(other._lifecycle);
#ifndef AERONET_LINUX
    _lastMaintenanceTp = other._lastMaintenanceTp;
#endif
    _router = std::move(other._router);
    _connections = std::move(other._connections);
    _keepAliveDeadlines = std::move(other._keepAliveDeadlines);
    _sharedBuffers = std::move(other._sharedBuffers);
    _telemetry = std::move(other._telemetry);
    _internalHandle = std::move(other._internalHandle);
    _lifecycleTracker = std::move(other._lifecycleTracker);
    _pendingReadFds = std::move(other._pendingReadFds);
    _connectionSweepState = std::exchange(other._connectionSweepState, {});
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
    _eventLoop = EventLoop(MakePollTimeoutPolicy(_config));
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
#ifdef AERONET_LINUX
    const bool ephemeralPort = _config.port == 0;
#endif
    _listenSocket.bindAndListen(_config.reusePort, _config.port);
    listenFd = _listenSocket.fd();
#ifdef AERONET_LINUX
    // The resolved ephemeral port is kept across restarts (rebound by the next initListener()), but the listener is
    // closed while stopped: another process could then obtain the port with bind(0), and with SO_REUSEPORT the restart
    // would silently share the incoming connections with it. Reserve it with a bound socket that never listens:
    // bind(0) never returns a port in use, and a socket that does not listen receives no connection.
    // Linux only: on other platforms, a bound socket that does not listen may be picked for incoming connections.
    if (ephemeralPort && _config.reusePort) {
      Socket reservation(Socket::Type::StreamNonBlock);
      if (reservation.tryBind(_config.reusePort, _config.port)) {
        _portReservation = std::move(reservation);
      }
    }
#endif
#ifdef AERONET_MACOS
  }
#endif

  _eventLoop.addOrThrow(EventLoop::EventFd{listenFd, EventIn});
  _eventLoop.addOrThrow(EventLoop::EventFd{_lifecycle.wakeupFd.fd(), EventIn});

  updateMaintenanceTimer();
  _eventLoop.addOrThrow(EventLoop::EventFd{_maintenanceTimer.fd(), EventIn});
}

void SingleHttpServer::prepareRun() {
  assert(_lifecycle.isStarting());
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
  _accessLog = AccessLogWriter(_config.accessLog);

  // Pre-clamp per-route limits against the global server limits so the hot path only needs a single comparison (no
  // runtime std::min).
  _router.clampConfigs(_config.maxHeaderBytes, _config.maxBodyBytes);

  _lifecycle.enterRunning();
}

void SingleHttpServer::beginStartup() {
  std::scoped_lock lock(_updates.lock);
  _lifecycle.enterStarting();
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

  EventLoopThreadRAII eventLoopThread(_lifecycle);
  LifecycleResetterRAII resetter(_lifecycle);

  prepareRun();

  while (_lifecycle.isActive() && !predicate()) {
    eventLoop();
  }

  // A predicate can become true after eventLoop() returns, bypassing the teardown normally performed inside it.
  // Close all network resources before leaving runUntil() on every platform. This must remain on the event-loop
  // thread because Windows WSAPoll registrations cannot be mutated safely by the controller thread.
  if (_lifecycle.isActive()) {
    closeListener();
    closeAllConnections();
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
    _lifecycle.reset();
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
  const auto prevState = _lifecycle.exchangeStopping();
  assert(prevState != internal::Lifecycle::State::Starting);
  if (prevState == internal::Lifecycle::State::Running || prevState == internal::Lifecycle::State::Draining) {
    // Wake the event loop immediately so it notices the Stopping state: the event-loop thread then closes the listener
    // and the connections and resets the lifecycle itself. None of them may be touched from here while it may still run
    // (it polls the listen socket, and a reset lifecycle would allow a restart while it is still running; on Windows,
    // closing the listen socket while WSAPoll holds it can even hang).
    _lifecycle.wakeupFd.send();

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
  const auto current = _lifecycle.waitWhileStarting();
  if (current == internal::Lifecycle::State::Idle || current == internal::Lifecycle::State::Stopping) {
    return;
  }

  if (maxWait.count() > 0) {
    // Set before entering Draining, so that the event loop never observes Draining without it. If the server is already
    // draining (possibly from a concurrent call), keeps the earliest of both deadlines.
    // Fresh clock read rather than the event loop's cached _connections.now: it is written by the event-loop thread,
    // and is stale (or still default-initialized) until the first poll() of a just-started server returns.
    _lifecycle.shrinkDeadline(std::chrono::steady_clock::now() + maxWait);
  }

  // Only from Running: a concurrent stop() (Stopping) must not be turned back into a drain.
  // The connections are owned by the event loop: their count cannot be read from here.
  if (current == internal::Lifecycle::State::Running && _lifecycle.enterDraining()) {
    log_noexcept::info("Initiating graceful drain");
  }
  // Keep listener open during drain to allow health probes to connect and receive 503 status.
  // Regular connections will still be accepted but will receive Connection: close headers.
}

void SingleHttpServer::registerBuiltInProbes() {
  if (!_config.builtinProbes.enabled) {
    return;
  }
  // True when this server is a MultiHttpServer worker whose probes are served by a dedicated probe listener.
  // Such workers skip registering the inline probe routes (they live on the dedicated port); the probe listener reads
  // their loop heartbeat (see internal::Lifecycle::loopHeartbeat, published by every event loop) to detect a wedge.
  // A standalone SingleHttpServer ignores dedicatedPort: it has no worker pool to isolate probes from.
  if (_config.builtinProbes.dedicatedPort != 0 && isInMultiHttpServer()) {
    // Probes are answered by MultiHttpServer's dedicated probe listener on its own port/thread; registering them
    // inline here too would just re-expose them on the (contended) application port, defeating the isolation.
    return;
  }

  // Liveness: Always returns 200 OK if the server is responding.
  // Indicates the application is alive (not deadlocked/crashed).
  _router.setPath(http::Method::GET, _config.builtinProbes.livenessPath(),
                  [](const HttpRequestView& req) { return req.makeResponse("OK"); });

  // Readiness: Returns 200 when ready to serve traffic, 503 during drain.
  // Used by load balancers to determine if instance should receive traffic.
  _router.setPath(http::Method::GET, _config.builtinProbes.readinessPath(), [this](const HttpRequestView& req) {
    const bool isReady = _lifecycle.ready();
    return req.makeResponse(isReady ? http::StatusCodeOK : http::StatusCodeServiceUnavailable,
                            isReady ? "OK" : "Not Ready");
  });

  // Startup: Returns 200 once server is running (listener active).
  // Note: Since aeronet has no separate initialization phase, this is essentially
  // equivalent to liveness. Provided for Kubernetes compatibility where startup
  // probes can have different timeout/period settings for slow-starting applications.
  _router.setPath(http::Method::GET, _config.builtinProbes.startupPath(), [this](const HttpRequestView& req) {
    const bool isStarted = _lifecycle.started();
    return req.makeResponse(isStarted ? http::StatusCodeOK : http::StatusCodeServiceUnavailable,
                            isStarted ? "OK" : "Starting");
  });
}

}  // namespace aeronet
