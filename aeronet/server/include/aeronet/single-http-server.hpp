#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <thread>

#include "aeronet/compiler-config.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/internal/connection-io.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/request-metrics.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router-update-proxy.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-callbacks.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/timer-fd.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/tcp-no-delay-mode.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-handshake-callback.hpp"
#endif

#ifdef AERONET_ENABLE_GLAZE
#include <string>

#include "aeronet/server-config-loader.hpp"
#endif

namespace aeronet {

class ServerLifecycleTracker;

// SingleHttpServer
//  - Single-threaded event loop by design: one instance == one epoll/reactor running in the
//    calling thread (typically the thread invoking run() / runUntil()).
//  - Not internally synchronized; do not access a given instance concurrently from multiple
//    threads (except destroying after stop()).
//  - To utilize multiple CPU cores, create several SingleHttpServer instances (possibly with
//    HttpServerConfig::withReusePort(true) on the same port) and run each in its own thread. Or better, use the
//    provided MultiHttpServer class made for this purpose.
//  - Writes currently assume exclusive ownership of the connection fd within this single
//    thread, enabling simple sequential ::write / ::writev without partial-write state tracking.
// Internally, it keeps the lifecycle, the listener, the dispatch of the polled events and the updates: the connections
// are served by the components of its internal::ServerContext (see there).
class SingleHttpServer {
 public:
  using ParserErrorCallback = aeronet::ParserErrorCallback;

  // Legacy alias kept for API compatibility.
  using RequestMetrics = aeronet::RequestMetrics;

  // Expectation handling API (see aeronet/server-callbacks.hpp).
  using ExpectationResultKind = aeronet::ExpectationResultKind;
  using ExpectationResult = aeronet::ExpectationResult;
  using ExpectationHandler = aeronet::ExpectationHandler;

  using MetricsCallback = aeronet::MetricsCallback;

  // AsyncHandle: RAII wrapper for non-blocking server execution
  // ------------------------------------------------------------
  // Returned by start() methods to manage the background thread running the SingleHttpServer event loop.
  // Provides lifetime management (RAII join on destruction) and error propagation from the background thread.
  //
  // Typical usage:
  //   SingleHttpServer server(cfg, router);
  //   auto handle = server.start();  // non-blocking
  //   // ... do work while server runs in background ...
  //   handle.stop();  // or let handle destructor auto-stop
  //   handle.rethrowIfError();  // check for exceptions from event loop
  class AsyncHandle {
   public:
    AsyncHandle(const AsyncHandle&) = delete;
    AsyncHandle& operator=(const AsyncHandle&) = delete;

    AsyncHandle(AsyncHandle&&) noexcept = default;
    AsyncHandle& operator=(AsyncHandle&&) noexcept = default;

    // Destructor automatically stops and joins the background thread (RAII)
    ~AsyncHandle() { stop(); }

    // Stop the background event loop and join the thread (blocking).
    // Safe to call multiple times; subsequent calls are no-ops.
    void stop() noexcept;

    // Rethrow any exception that occurred in the background event loop.
    // Call after stop() to check for errors.
    void rethrowIfError();

    // Check if the background thread is still active (not yet joined).
    [[nodiscard]] bool started() const noexcept { return _thread.joinable(); }

   private:
    friend class SingleHttpServer;

    AsyncHandle() noexcept = default;
    AsyncHandle(std::jthread thread, std::shared_ptr<std::exception_ptr> error);

    std::jthread _thread;
    std::shared_ptr<std::exception_ptr> _error;  // shared for lambda capture
  };

  // Construct an SingleHttpServer with a default configuration that does not immediately starts listening.
  // As a consequence, the ephemeral port is NOT allocated at this time and port() will return 0.
  SingleHttpServer() noexcept;

  // Construct a server bound and listening immediately according to given configuration.
  //  - Performs: ::socket, setsockopt (REUSEADDR always, REUSEPORT best-effort if enabled), ::bind, ::listen,
  //    retrieves (and overwrites cfg.port with) the chosen ephemeral port if cfg.port == 0, sets O_NONBLOCK,
  //    and registers the listening fd with the internal EventLoop.
  //  - If any step fails it throws std::runtime_error (leaving no open fd).
  //  - After construction port() returns the actual bound port (deterministic for tests using ephemeral ports).
  explicit SingleHttpServer(HttpServerConfig config, RouterConfig routerConfig = {});

  // Constructs a server bound and listening immediately according to given configuration,
  // and using the provided Router for request routing (can be configured after construction before run).
  SingleHttpServer(HttpServerConfig cfg, Router router);

#ifdef AERONET_ENABLE_GLAZE
  // Construct a server from a JSON or YAML configuration file.
  // Format is auto-detected from file extension (.json, .yaml, .yml).
  // Router can then be configured with the router() method.
  // Throws std::runtime_error on parse or file I/O errors.
  explicit SingleHttpServer(const std::filesystem::path& configPath);

  // Construct a server from a JSON or YAML configuration file while reusing an already-configured Router.
  // Only the HttpServerConfig portion of the file is applied; the RouterConfig from the file is ignored.
  // This preserves all handlers/routes already registered in `router`.
  // Format is auto-detected from file extension (.json, .yaml, .yml).
  // Throws std::runtime_error on parse or file I/O errors.
  SingleHttpServer(const std::filesystem::path& configPath, Router router);
#endif

  // A SingleHttpServer is copyable - but only from a non-running instance.
  // The copy will duplicate the configuration and router state, but not
  // any active connections or runtime state.
  // sharedListenFd is used by MultiHttpServer on macOS where SO_REUSEPORT does not load-balance loopback traffic.
  // The caller retains ownership of the socket; this instance registers EVFILT_READ on it
  // in its own kqueue, enabling thundering-herd accept() distribution.
  SingleHttpServer(const SingleHttpServer& other, NativeHandle sharedListenFd = kInvalidHandle);

  // Copy-assignment mirrors the copy-constructor semantics: the source must be fully stopped while the
  // destination is stopped (stop() is invoked internally before applying the copy). Attempts to copy-assign
  // from a running instance throw std::logic_error to avoid duplicating live event loops / sockets.
  SingleHttpServer& operator=(const SingleHttpServer& other);

  // Move semantics & constraints:
  // -----------------------------
  // A SingleHttpServer can be moved ONLY when it is not running. Attempting to move (construct or assign from) a
  // running server is a logic error: the event loop thread would continue executing against the old "this"
  // while ownership of its internal epoll fd, listening socket, wakeup fd, connection maps, handlers and TLS
  // context had been transferred, leading to immediate undefined behaviour. To make this failure mode explicit
  // and prevent silent partial moves, the move constructor and move assignment operator throws
  // std::logic_error if the source object is running.
  //
  // Design choice:
  //  * We intentionally drop noexcept on move operations to surface misuse instead of asserting and then
  //    forcing a partially moved/stopped state.
  //  * This keeps normal (non‑running) moves available for ergonomic construction & storage patterns.
  //
  // Safe usage pattern:
  //    Router router;
  //    router.setDefault(...);
  //    SingleHttpServer tmp(cfg, std::move(router));
  //    SingleHttpServer server(std::move(tmp)); // OK (tmp not running)
  //    std::jthread t([&]{ server.run(); });
  //
  // Invalid usage (throws std::runtime_error):
  //    SingleHttpServer s(cfg);
  //    std::jthread t([&]{ s.run(); });
  //    SingleHttpServer moved(std::move(s)); // throws
  SingleHttpServer(SingleHttpServer&& other);             // NOLINT(performance-noexcept-move-constructor)
  SingleHttpServer& operator=(SingleHttpServer&& other);  // NOLINT(performance-noexcept-move-constructor)

  // Stops the server. Waits for the deferred work (HttpRequestView::deferWork()) still running: it uses the coroutine
  // frames and requests kept for it, and completes by posting to this server.
  ~SingleHttpServer();

  // Obtain a proxy enabling fluent router updates without accessing the router directly while running.
  // Allows call chaining and implicit conversion to Router& for inspection during setup.
  RouterUpdateProxy router();

  // Install a callback invoked whenever the request parser encounters a non‑recoverable
  // protocol error for a connection. Typical causes correspond to the HTTP status codes.
  //
  // Semantics:
  //   - Callback is executed in the server's event loop thread just before the server
  //     generates and queues an error response (usually 400 / 413 / 431 depending on case).
  //   - Keep the body extremely light (metrics increment, logging). Avoid blocking or heavy
  //     allocations – it delays processing of other connections.
  //   - The connection may be closed after the error response depending on the nature of the
  //     failure (e.g. malformed framing).
  //
  // Lifetime:
  //   - May be set or replaced at any time; the latest callback is used for subsequent parse
  //     failures. Provide an empty std::function ({} ) to clear.
  //   - While the server is running, a call from another thread waits for the event loop to install it (like
  //     all the callback setters below).
  //
  // Exceptions:
  // - Exceptions escaping the callback are caught and ignored to preserve server stability.
  void setParserErrorCallback(ParserErrorCallback cb);

  // Install a callback invoked after completing each request (including errors).
  // Semantics:
  //   - Callback is executed in the server's event loop thread after the response
  //     has been fully sent.
  //   - Keep the body extremely light (metrics increment, logging). Avoid blocking or heavy
  //     allocations – it delays processing of other connections.
  // Lifetime:
  //   - May be set or replaced at any time; the latest callback is used for subsequent requests.
  //     Provide an empty std::function ({}) to clear.
  void setMetricsCallback(MetricsCallback cb);

#ifdef AERONET_ENABLE_OPENSSL
  // Install a callback invoked exactly once per connection for TLS handshake outcomes.
  // Semantics:
  //   - Executed in the server's event loop thread.
  //   - Called on success, failure (including mTLS verification failures), and admission-control rejection.
  //   - All string_view fields are only guaranteed valid for the duration of the callback.
  // Exceptions:
  //   - Exceptions escaping the callback are caught and ignored.
  // Lifetime: same as setParserErrorCallback().
  void setTlsHandshakeCallback(TlsHandshakeCallback cb);
#endif

  // Register or clear the expectation handler. This handler will be invoked from
  // the server's event-loop thread when a request contains an `Expect` header
  // with tokens other than "100-continue". To clear, pass an empty std::function.
  // Lifetime: same as setParserErrorCallback().
  void setExpectationHandler(ExpectationHandler handler);

  // Install a callback invoked with middleware metrics. Lifetime: same as setParserErrorCallback().
  void setMiddlewareMetricsCallback(MiddlewareMetricsCallback cb);

  // Run the server event loop until stop() is called (e.g. from another thread) or the process receives SIGINT/SIGTERM.
  // The maximum blocking interval of a single poll cycle is controlled by HttpServerConfig::pollInterval.
  // This method is blocking for the caller thread.
  void run();

  // Run the server until the user-supplied predicate returns true (checked once per loop iteration) or stop() is
  // invoked / signal received. Semantics of checkPeriod are identical to run(): it is the upper bound on how long we
  // may block waiting for new events when idle. The predicate is evaluated after processing any ready events and
  // before the next epoll_wait call. A very small checkPeriod will evaluate the predicate more frequently, but at the
  // cost of additional wake‑ups when idle.
  // Like run() (so method is blocking for the current thread) but exits when the user-supplied predicate returns true
  // (checked once per loop iteration) or stop() is invoked / signal received. Poll sleep upper bound is
  // HttpServerConfig::pollInterval.
  void runUntil(const std::function<bool()>& predicate);

  // start():
  //   Launch the event loop in a background thread. The server manages the thread lifetime
  //   internally and will automatically stop and join when the server is destroyed or stop()
  //   is called.
  //
  // Usage:
  //   SingleHttpServer server(cfg, router);
  //   server.start();  // non-blocking, returns void
  //   // ... server runs in background ...
  //   // Destructor or stop() will clean up automatically
  void start();

  // startDetached():
  //   Like start(), but returns an AsyncHandle for explicit lifetime management.
  //   Use this when you need fine-grained control (checking if started, rethrowIfError, etc).
  //   The AsyncHandle will automatically stop and join the thread on destruction (RAII).
  //
  // Usage:
  //   SingleHttpServer server(cfg, router);
  //   auto handle = server.startDetached();  // non-blocking
  //   // ... server runs in background ...
  //   handle.stop();  // or let destructor handle it
  //   handle.rethrowIfError();
  [[nodiscard]] AsyncHandle startDetached() { return launchDetached(); }

  // startDetachedAndStopWhen():
  //   Like startDetached(), but the event loop will also terminate when the provided predicate returns true.
  //   The predicate is checked once per loop iteration alongside the stop token check.
  //
  // Usage:
  //   std::atomic<bool> done{false};
  //   auto handle = server.startDetachedAndStopWhen([&]{ return done.load(); });
  //   // ... later ...
  //   done = true;  // triggers shutdown
  [[nodiscard]] AsyncHandle startDetachedAndStopWhen(std::function<bool()> predicate);

  // startDetachedWithStopToken():
  //   Launch the event loop that stops when either the provided std::stop_token or the
  //   AsyncHandle's internal token requests stop. Useful for integrating with external
  //   cooperative cancellation mechanisms.
  //
  // Usage:
  //   std::stop_source source;
  //   auto handle = server.startDetachedWithStopToken(source.get_token());
  //   // ... later ...
  //   source.request_stop();  // triggers shutdown
  [[nodiscard]] AsyncHandle startDetachedWithStopToken(std::stop_token token);

  // Requests cooperative termination of the event loop. Safe to invoke from a different thread, or from a handler.
  // The event loop is woken up immediately: it then closes the listening socket and the connections itself, and
  // run()/runUntil() return.
  // Usually called from a different thread than the one that started the server, this method is not blocking,
  // so the server might not be immediately stopped once the method returns to the caller: join the thread running it
  // before restarting or destroying the server. Exception: when started with start(), the event-loop thread is joined
  // (unless stop() is called from it).
  // Note that you can also call stop on a server that listens on a port without being running - in this case, it will
  // close the listening socket.
  //
  // Idempotency:
  //   - Repeated calls are harmless. Calling after the server already stopped has no effect.
  //
  // Typical usage:
  //   - From a signal handler wrapper (set a flag then call stop() in a safe context).
  //   - From a controller thread coordinating multiple SingleHttpServer instances.
  // Note: it is possible to call 'run()' again on a stopped server.
  void stop() noexcept;

  // Initiate graceful draining: stop accepting new connections, and close existing keep-alive
  // sessions after their current in-flight response completes. When maxWait > 0 a deadline is
  // enforced, after which remaining connections are closed immediately. Safe to call from a
  // different thread. Calling beginDrain() while already draining updates the deadline to the
  // earliest of the current and new values. If the server is still starting up (right after
  // start()), waits for its startup to complete so that the drain is not lost.
  void beginDrain(std::chrono::milliseconds maxWait = std::chrono::milliseconds{0}) noexcept;

  // The config given to the server, with the actual allocated port if 0 was given.
  // The config may be modified in-flight via postConfigUpdate.
  [[nodiscard]] const HttpServerConfig& config() const { return _ctx.config; }

  // Get the actual port of this server.
  // If the configuration port was 0, the port has been automatically allocated by the system.
  [[nodiscard]] uint16_t port() const { return _ctx.config.port; }

#ifdef AERONET_ENABLE_GLAZE
  // Serialize the current server and router configuration as a JSON or YAML string.
  [[nodiscard]] std::string dumpConfig(ConfigFormat format = ConfigFormat::json) const;

  // Write the current server and router configuration to a file.
  // Format is auto-detected from the file extension (.json, .yaml, .yml).
  void saveConfig(const std::filesystem::path& filePath) const;
#endif

  // Returns true while the event loop is actively executing inside run() / runUntil(), and
  // false otherwise (before start, after stop(), or after loop exit due to predicate / error).
  // Because the lifecycle state is mutated from the run loop and external control methods,
  // concurrent observers may observe a short delay (bounded by pollInterval) before noticing
  // the transition. Primarily intended for coarse-grained coordination / diagnostics, not for
  // high-precision synchronization. For deterministic shutdown sequencing prefer joining the
  // thread that called run()/runUntil().
  [[nodiscard]] bool isRunning() const { return _ctx.lifecycle.isRunning(); }

  // Returns true if the server is in draining mode (no new connections accepted, existing keep-alive
  // connections closed after current response), false otherwise.
  [[nodiscard]] bool isDraining() const { return _ctx.lifecycle.isDraining(); }

  // Retrieve current server statistics snapshot. Safe to call from any thread, also while running.
  [[nodiscard]] ServerStats stats() const;

  // Post a configuration update to be applied safely from the server's event loop thread.
  // Semantics (simple): the updater is appended to an internal queue and will be applied
  // at the beginning of the next event loop iteration on the server thread. If the server
  // is stopped, the updater is retained and will be applied in the first event loop run.
  //
  // Immutability protection:
  //   The following fields are immutable (require socket rebind or structural reinitialization)
  //   and will be silently restored to their original values after the updater runs:
  //     - port, reusePort (socket binding parameters)
  //     - otel (tracer/exporter setup is one-time at construction)
  //   Attempting to modify these fields will have no effect and will emit a warning log in
  //   debug builds.
  //
  // Failure handling:
  //   Each posted update is applied atomically. If the updater throws, if the resulting config fails validation, or if
  //   an object built from it cannot be created (TLS context: unreadable or invalid certificate, key, OCSP response or
  //   CRL; access log file), the error is logged and the previous config (and TLS context) is kept. The other updates
  //   posted at the same time are applied independently.
  //
  // TLS hot reload:
  //   TLS configuration (cfg.tls) IS mutable via postConfigUpdate. When you modify cfg.tls,
  //   the server will automatically rebuild the SSL context for new connections. Existing
  //   connections continue using their current SSL/TLS state.
  //   Each applied update also reloads the TLS files (certificate, key, OCSP response, CRL, SNI ones included) that
  //   changed on disk since the TLS context was built, even if their paths did not change: an empty update
  //   (postConfigUpdate([](HttpServerConfig&) {})) is the way to reload TLS files rotated in place. A change is
  //   detected with stat() (device, inode, size, modification and status change times, following symbolic links). If
  //   the reload fails, the previous TLS context is kept and the reload is attempted again at the next update.
  //
  // All other fields (limits, timeouts, compression settings, etc.) are mutable and will
  // take effect as documented for each field.
  void postConfigUpdate(std::function<void(HttpServerConfig&)> updater);

  // Post a router update to be applied from the event loop thread. The updater executes with exclusive
  // access to the Router immediately before the next loop iteration, ensuring thread-safety for handler updates.
  void postRouterUpdate(std::function<void(Router&)> updater);

 private:
  friend class MultiHttpServer;

  using ConnectionIt = internal::ConnectionStorage::ConnectionIt;
  using CloseStatus = internal::CloseStatus;

  // The context of 'other', after checking that it can be copied from (it does not run).
  static const internal::ServerContext& CopyableContext(const SingleHttpServer& other);

  // The context of 'other', after checking that it can be moved from (it does not run, and no deferred work still runs
  // for its closed connections).
  static internal::ServerContext&& MovableContext(SingleHttpServer& other);

  void initListener(NativeHandle listenFd = kInvalidHandle);

  // Leaves Idle for Starting under the updates lock: an update applied right away because the server is stopped (see
  // submitRouterUpdate(), submitCallbacksUpdate()) then completes before the startup, every later one being queued
  // for the event loop.
  void beginStartup();

  void prepareRun();

  void runUntilStarted(const std::function<bool()>& predicate);

  // Shared implementation for startDetached / startDetachedAndStopWhen / startDetachedWithStopToken.
  AsyncHandle launchDetached(std::function<bool()> extraPredicate = {});

  void eventLoop();

  // Handles the event of a connection polled at 'now'. Inlined into eventLoop(), its only caller.
  AERONET_ALWAYS_INLINE void handleConnectionEvent(EventLoop::EventFd event, std::chrono::steady_clock::time_point now);

  // Serves the input of the connections deferred by ConnectionIo::deferInput().
  void resumeDeferredInputs();

  // Periodic maintenance: timeouts sweep, access log flush, end of a stop or a drain.
  void runMaintenance(std::chrono::steady_clock::time_point now);

  void updateMaintenanceTimer();

  void closeListener() noexcept;

  // Closes the listener and all the connections, at the end of a run.
  void closeListenerAndConnections();

  void applyPendingUpdates();

  // Applies the posted config updates, each one atomically (restoring the previous configuration if it fails).
  void applyPendingConfigUpdates();

  void submitRouterUpdate(std::function<void(Router&)> updater,
                          std::shared_ptr<std::promise<std::exception_ptr>> completion);

  // Applies a change of the callbacks: right away when stopped or when called from the event loop, otherwise by the
  // event loop (the only thread using them while running), waiting for it (see applyPendingUpdates()).
  void submitCallbacksUpdate(std::function<void(internal::ServerCallbacks&)> updater);

  // Applies the changes queued by submitCallbacksUpdate(). PRECONDITION: updates lock held, server stopped.
  void applyQueuedCallbacksUpdates();

  void registerBuiltInProbes();

  void acceptNewConnections();

  CloseStatus handleWritableClient(ConnectionIt cnxIt);

  // Reads and processes available input of a connection: the only read loop of the HTTP connections (tunnels excepted),
  // from the read right after the accept on.
  // stopOnShortRead: no hang-up / error can be pending unreported, so a short plain-socket read proves the kernel
  // receive queue drained and the final read that would only return EAGAIN can be skipped (edge-triggered polling
  // reports any later data). True for an event without hang-up / error, and for the read right after the accept
  // (registering the socket reports what it already received). Pass false when the event may carry a pending EOF, or
  // when the read resumes input whose events were already consumed.
  CloseStatus handleReadableClient(ConnectionIt cnxIt, bool stopOnShortRead);

  // Ends the handling of the connection fd outside of an event of the polled batch (right after its accept, or when its
  // deferred input is resumed): closes it if requested - unless a nested close already recycled it - or restarts its
  // keep-alive idle window otherwise.
  void completeOutOfBatchRead(NativeHandle fd, CloseStatus closeStatus);

  // Finalize a TLS handshake if it just completed: emit metrics/events, set up HTTP/2
  // if ALPN negotiated "h2", and mark the connection as TLS-established.
  // Returns true if the connection requested close (caller should close/return).
  bool finalizeTlsHandshakeIfReady(NativeHandle fd, ConnectionState& state);

  // Dispatches input to the protocol of the connection: its protocol handler (WebSocket, HTTP/2), HTTP/2 with prior
  // knowledge, or HTTP/1.1. Returns true if the connection should stop reading (or close).
  bool processConnectionInput(ConnectionIt cnxIt);

  // Processes the input of a connection served by a protocol handler (see processSpecialProtocolHandler()), then
  // re-arms the deadline of a WebSocket connection. Returns true if the connection should be closed.
  bool processProtocolHandlerInput(ConnectionIt cnxIt);

  // Serves the HTTP/1.1 requests in the input buffer of the connection (see Http1Pipeline::processRequests()), then
  // gives the bytes following an upgrade request to the new protocol.
  // Returns true if the connection should stop reading (or close).
  bool processHttp1Input(ConnectionIt cnxIt);

  // Process WebSocket / HTTP/2 data through the protocol handler.
  // Returns true if the connection should be closed.
  bool processSpecialProtocolHandler(ConnectionIt cnxIt);

  // Tells whether the protocol handler input processing must stop: close requested, or output blocked.
  static bool StopReadingIfOutputBlocked(ConnectionState& state);

  // Serves the input already in the input buffer of a connection whose output is not blocked: the complete requests
  // (HTTP/2 frames) left by the output backpressure or the fairness budget, that no event reports.
  void processBufferedInput(NativeHandle fd);

  // Resumes the input of a connection deferred by ConnectionIo::deferInput(): serves its buffered input, then reads.
  CloseStatus resumeDeferredInput(ConnectionIt cnxIt);

  // Restarts the keep-alive idle windows of the connections of a processed batch of events, read at batchStart: a
  // single clock read when the whole batch was served within keepAliveTimeout, the common case.
  void restartKeepAliveIdleWindows(std::span<const EventLoop::EventFd> events,
                                   std::chrono::steady_clock::time_point batchStart);

  // Listener, wakeup and maintenance timer fds, polled along with the connections.
  [[nodiscard]] bool isServerInternalFd(NativeHandle fd) const noexcept;

#ifdef AERONET_ENABLE_HTTP2
  // Sets up HTTP/2 protocol handler for a connection after ALPN "h2" negotiation.
  // Initializes the protocol handler, sends the server preface (SETTINGS frame),
  // installs CONNECT tunnel bridge, and flushes output.
  void setupHttp2Connection(NativeHandle clientFd, TcpNoDelayMode tcpNoDelayMode, ConnectionState& state);

  // Installs the HTTP/2 protocol handler of a connection (ALPN "h2" or h2c prior knowledge), with its CONNECT tunnel
  // bridge and its callbacks.
  void installHttp2Handler(NativeHandle clientFd, ConnectionState& state, bool sendServerPrefaceForTls);
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Processes the async callbacks posted by background threads: resumes their coroutines, or releases the ones kept for
  // closed connections.
  void processAsyncCallbacks();

  // Waits until the deferred work run for closed connections completed (no-op while the event loop runs).
  void waitForOrphanedAsyncWork();

  // Resumes the input of an HTTP/1 connection after the completion of its async handler: serves the requests already
  // received, then reads the bytes left in the socket meanwhile.
  void resumeInputAfterAsyncHandler(NativeHandle fd);
#endif

  [[nodiscard]] bool isInMultiHttpServer() const noexcept { return _lifecycleTracker.use_count() != 0; }

  internal::ServerContext _ctx;

  // Callback changes queued by submitCallbacksUpdate() (protected by _ctx.updates.lock).
  vector<std::function<void(internal::ServerCallbacks&)>> _callbacksUpdates;
  std::atomic<bool> _hasCallbacksUpdates{false};

  Socket _listenSocket;
#ifdef AERONET_LINUX
  // Linux only: bound (never listening) socket keeping an ephemeral port reserved for the whole life of this server
  // when reusePort is enabled, see initListener(). Not copied: it stays with the server that resolved the port.
  Socket _portReservation;
#endif
  TimerFd _maintenanceTimer;

#ifndef AERONET_LINUX
  // Timestamp of last maintenance sweep. Used as fallback on Windows/macOS where the
  // TimerFd does not fire real events; ensures sweepIdleConnections runs at least once
  // per pollInterval even under sustained event activity.
  std::chrono::steady_clock::time_point _lastMaintenanceTp;
#endif

  // Internal handle for simple start() API - managed by the server itself.
  // When start() is called, the handle is stored here and the server takes ownership.
  // When startDetached() is called, the handle is returned to the caller.
  AsyncHandle _internalHandle;

  // Used by MultiHttpServer to track lifecycle without strong ownership.
  std::weak_ptr<ServerLifecycleTracker> _lifecycleTracker;

#ifdef AERONET_MACOS
  // When true (default), this instance owns the listen socket and will close it
  // on stop/destruction. When false, the socket is borrowed from another instance
  // (shared-fd model on macOS) and must not be closed by this instance.
  bool _ownsListenSocket{true};
#endif
};

}  // namespace aeronet
