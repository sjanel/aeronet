#pragma once

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string_view>
#include <thread>

#include "aeronet/connection-state.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/lifecycle.hpp"
#include "aeronet/internal/pending-updates.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router-update-proxy.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/timer-fd.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-protocol-handler.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/internal/tls-runtime-state.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#endif

namespace aeronet {

class ServerLifecycleTracker;

class HttpResponseWriter;
class CorsPolicy;

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
class SingleHttpServer {
 public:
  using ParserErrorCallback = std::function<void(http::StatusCode)>;

  struct RequestMetrics {
    http::StatusCode status{0};
    http::Method method;
    bool reusedConnection{false};
    std::string_view path;
    std::size_t bytesIn{0};
    std::size_t bytesOut{0};
    std::chrono::nanoseconds duration{0};
  };

  // Expectation handling API
  // ------------------------
  // The server will honour the standard "Expect: 100-continue" behaviour by default.
  // For other Expect tokens, applications may register an ExpectationHandler to
  // implement custom semantics (for example sending an interim 102 Processing
  // or producing a final response). If no handler is registered and the Expect
  // header contains any token other than "100-continue", the server will respond
  // with 417 Expectation Failed per RFC and reject the request.
  enum class ExpectationResultKind : uint8_t { Continue, Interim, FinalResponse, Reject };

  struct ExpectationResult {
    ExpectationResultKind kind = ExpectationResultKind::Continue;
    // Used for Interim when the handler wants the server to emit an interim
    // response with the given status code (e.g. 102). Stored as uint8_t because
    // only the 1xx class (100-199) is valid and fits in one byte.
    uint8_t interimStatus = 0;
    // Used for FinalResponse when the handler wishes to reply immediately
    // with a full HttpResponse (the server will send it and skip reading the body).
    HttpResponse finalResponse;
  };

  using ExpectationHandler = std::function<ExpectationResult(const HttpRequest&, std::string_view)>;

  using MetricsCallback = std::function<void(const RequestMetrics&)>;

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
    ~AsyncHandle();

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
  SingleHttpServer() noexcept = default;

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

  // A SingleHttpServer is copyable - but only from a non-running instance.
  // The copy will duplicate the configuration and router state, but not
  // any active connections or runtime state.
  SingleHttpServer(const SingleHttpServer& other);

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

  ~SingleHttpServer() { stop(); }

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
  void setTlsHandshakeCallback(TlsHandshakeCallback cb);
#endif

  // Register or clear the expectation handler. This handler will be invoked from
  // the server's event-loop thread when a request contains an `Expect` header
  // with tokens other than "100-continue". To clear, pass an empty std::function.
  void setExpectationHandler(ExpectationHandler handler);

  // Install a callback invoked with middleware metrics.
  void setMiddlewareMetricsCallback(MiddlewareMetricsCallback cb) { _callbacks.middlewareMetrics = std::move(cb); }

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
  [[nodiscard]] AsyncHandle startDetached();

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

  // Requests cooperative termination of the event loop. Safe to invoke from a different thread
  // (best‑effort). The maximum observable latency before run()/runUntil() return is bounded by
  // the checkPeriod supplied to those functions (epoll returns earlier if events arrive).
  // New incoming connections are prevented by closing the listening socket immediately;
  // existing established connections are not force‑closed – they simply stop being serviced once the loop exits.
  // Usually called from a different thread than the one that started the server, this method is not blocking,
  // so the server might not be immediately stopped once the method returns to the caller.
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
  // earliest of the current and new values.
  void beginDrain(std::chrono::milliseconds maxWait = std::chrono::milliseconds{0}) noexcept;

  // The config given to the server, with the actual allocated port if 0 was given.
  // The config may be modified in-flight via postConfigUpdate.
  [[nodiscard]] const HttpServerConfig& config() const { return _config; }

  // Get the actual port of this server.
  // If the configuration port was 0, the port has been automatically allocated by the system.
  [[nodiscard]] uint16_t port() const { return _config.port; }

  // Returns true while the event loop is actively executing inside run() / runUntil(), and
  // false otherwise (before start, after stop(), or after loop exit due to predicate / error).
  // Because the lifecycle state is mutated from the run loop and external control methods,
  // concurrent observers may observe a short delay (bounded by pollInterval) before noticing
  // the transition. Primarily intended for coarse-grained coordination / diagnostics, not for
  // high-precision synchronization. For deterministic shutdown sequencing prefer joining the
  // thread that called run()/runUntil().
  [[nodiscard]] bool isRunning() const { return _lifecycle.isRunning(); }

  // Returns true if the server is in draining mode (no new connections accepted, existing keep-alive
  // connections closed after current response), false otherwise.
  [[nodiscard]] bool isDraining() const { return _lifecycle.isDraining(); }

  // Access the telemetry context for custom tracing/spans.
  // The returned reference is valid for the lifetime of the SingleHttpServer instance,
  // but is invalidated if the server is moved.
  [[nodiscard]] const tracing::TelemetryContext& telemetryContext() const noexcept { return _telemetry; }

  // Retrieve current server statistics snapshot.
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
  // TLS hot reload:
  //   TLS configuration (cfg.tls) IS mutable via postConfigUpdate. When you modify cfg.tls,
  //   the server will automatically rebuild the SSL context for new connections. Existing
  //   connections continue using their current SSL/TLS state. If the new config is invalid
  //   or context rebuild fails, the old TLS context remains active and the config is restored.
  //
  // All other fields (limits, timeouts, compression settings, etc.) are mutable and will
  // take effect as documented for each field.
  void postConfigUpdate(std::function<void(HttpServerConfig&)> updater);

  // Post a router update to be applied from the event loop thread. The updater executes with exclusive
  // access to the Router immediately before the next loop iteration, ensuring thread-safety for handler updates.
  void postRouterUpdate(std::function<void(Router&)> updater);

 private:
  friend class HttpResponseWriter;  // allow streaming writer to access queueData and _connStates
  friend class MultiHttpServer;

  using ConnectionMapIt = internal::ConnectionStorage::ConnectionMapIt;

  void initListener();
  void prepareRun();

  void eventLoop();
  void sweepIdleConnections();
  void applyPendingUpdates();
  void acceptNewConnections();

  void handleWritableClient(int fd);
  void handleReadableClient(int fd);

  // Dispatches input to appropriate handler based on protocol.
  // For HTTP/1.1, calls processHttp1Requests.
  // For WebSocket, routes through the protocol handler.
  // Returns true if the connection should be closed.
  bool processConnectionInput(ConnectionMapIt cnxIt);
  bool processHttp1Requests(ConnectionMapIt cnxIt);
  // Process WebSocket data through the protocol handler.
  // Returns true if the connection should be closed.
  bool processSpecialProtocolHandler(ConnectionMapIt cnxIt);
  // Split helpers
  enum class BodyDecodeStatus : uint8_t { Ready, NeedMore, Error };

  BodyDecodeStatus decodeBodyIfReady(ConnectionMapIt cnxIt, bool isChunked, bool expectContinue,
                                     std::size_t& consumedBytes);
  BodyDecodeStatus decodeFixedLengthBody(ConnectionMapIt cnxIt, bool expectContinue, std::size_t& consumedBytes);
  BodyDecodeStatus decodeChunkedBody(ConnectionMapIt cnxIt, bool expectContinue, std::size_t& consumedBytes);
  bool parseHeadersUnchecked(HeadersViewMap& headersMap, char* bufferBeg, char* first, char* last);
  bool maybeDecompressRequestBody(ConnectionMapIt cnxIt);
  void finalizeAndSendResponseForHttp1(ConnectionMapIt cnxIt, HttpResponse&& resp, std::size_t consumedBytes,
                                       const CorsPolicy* pCorsPolicy);
  // Handle Expect header tokens other than the built-in 100-continue.
  // Returns true if processing should stop for this request (response already queued/sent).
  bool handleExpectHeader(ConnectionMapIt cnxIt, std::string_view expectHeader, const CorsPolicy* pCorsPolicy,
                          bool& found100Continue);
  // Helper to populate and invoke the metrics callback for a completed request.
  void emitRequestMetrics(const HttpRequest& request, http::StatusCode status, std::size_t bytesIn,
                          bool reusedConnection) const;

  // Helper to build & queue a simple error response, invoke parser error callback (if any).
  // If immediate=true the connection will be closed without waiting for buffered writes to drain.
  void emitSimpleError(ConnectionMapIt cnxIt, http::StatusCode statusCode, bool immediate = false,
                       std::string_view body = {});
  // Outbound write helpers. TODO: check return values for all callers, or just close connection on failure?
  bool queueData(ConnectionMapIt cnxIt, HttpResponseData httpResponseData);
  void flushOutbound(ConnectionMapIt cnxIt);
  void flushFilePayload(ConnectionMapIt cnxIt);
  // Helper: flush pending bytes in tunnelOrFileBuffer via user-space TLS (SSL_write).
  // Used when kTLS is not available and file data must be encrypted in user-space.
  // Returns true if the caller should return early because the buffer is still non-empty.
  bool flushUserSpaceTlsBuffer(ConnectionMapIt cnxIt);

  ConnectionMapIt closeConnection(ConnectionMapIt cnxIt);

  // Invoke a registered streaming handler. Returns true if the connection should be closed after handling
  // the request (either because the client requested it or keep-alive limits reached). The HttpRequest is
  // non-const because we may reuse shared response finalization paths (e.g. emitting a 406 early) that expect
  // to mutate transient fields (target normalization already complete at this point).
  bool callStreamingHandler(const StreamingHandler& streamingHandler, ConnectionMapIt cnxIt, std::size_t consumedBytes,
                            const CorsPolicy* pCorsPolicy, std::span<const ResponseMiddleware> postMiddleware);

  enum class LoopAction : uint8_t { Nothing, Continue, Break };

  LoopAction processSpecialMethods(ConnectionMapIt& cnxIt, std::size_t consumedBytes, const CorsPolicy* pCorsPolicy);

  // HTTP/1.1-specific CONNECT handling (TCP tunnel setup).
  LoopAction processConnectMethod(ConnectionMapIt& cnxIt, std::size_t consumedBytes, const CorsPolicy* pCorsPolicy);

  void handleInTunneling(ConnectionMapIt cnxIt);

  void closeListener() noexcept;
  void closeAllConnections();

  void registerBuiltInProbes();

  void updateMaintenanceTimer();

  void submitRouterUpdate(std::function<void(Router&)> updater,
                          std::shared_ptr<std::promise<std::exception_ptr>> completion);

  // Helpers to enable/disable writable interest (EPOLLOUT) for a connection. They wrap
  // ModWithCloseOnFailure and update `ConnectionState::waitingWritable` and internal stats
  // consistently. Return true on success, false on failure (caller should handle close).
  bool enableWritableInterest(ConnectionMapIt cnxIt);
  bool disableWritableInterest(ConnectionMapIt cnxIt);

  bool dispatchAsyncHandler(ConnectionMapIt cnxIt, const AsyncRequestHandler& handler, bool bodyReady, bool isChunked,
                            bool expectContinue, std::size_t consumedBytes, const CorsPolicy* pCorsPolicy,
                            std::span<const ResponseMiddleware> responseMiddleware);
  void resumeAsyncHandler(ConnectionMapIt cnxIt);
  void handleAsyncBodyProgress(ConnectionMapIt cnxIt);
  void onAsyncHandlerCompleted(ConnectionMapIt cnxIt);
  void tryFlushPendingAsyncResponse(ConnectionMapIt cnxIt);

  [[nodiscard]] bool isInMultiHttpServer() const noexcept { return _lifecycleTracker.use_count() != 0; }

#ifdef AERONET_ENABLE_HTTP2
  // Sets up HTTP/2 protocol handler for a connection after ALPN "h2" negotiation.
  // Initializes the protocol handler, sends the server preface (SETTINGS frame), and flushes output.
  void setupHttp2Connection(ConnectionState& state);
#endif

  struct StatsInternal {
    uint64_t totalBytesQueued{0};
    uint64_t totalBytesWrittenImmediate{0};
    uint64_t totalBytesWrittenFlush{0};
    uint64_t deferredWriteEvents{0};
    uint64_t flushCycles{0};
    uint64_t epollModFailures{0};
    std::size_t maxConnectionOutboundBuffer{0};
    uint64_t totalRequestsServed{0};
  } _stats;

  struct Callbacks {
    ParserErrorCallback parserErr;
    MetricsCallback metrics;
    MiddlewareMetricsCallback middlewareMetrics;
#ifdef AERONET_ENABLE_OPENSSL
    TlsHandshakeCallback tlsHandshake;
#endif
    ExpectationHandler expectation;
  } _callbacks;

  internal::PendingUpdates _updates;

  internal::ResponseCompressionState _compression;

  HttpServerConfig _config;

  Socket _listenSocket;
  TimerFd _maintenanceTimer;
  EventLoop _eventLoop;

  internal::Lifecycle _lifecycle;

  Router _router;

  internal::ConnectionStorage _connections;

  struct TempBuffers {
    RawChars buf;                 // can be used for any kind of temporary buffer
    RawChars32 trailers;          // scratch buffer to preserve request trailers during decompression
    vector<std::string_view> sv;  // scratch vector for chunked decoding
  } _tmp;

  // Telemetry context - one per SingleHttpServer instance (no global singletons)
  tracing::TelemetryContext _telemetry;

  // Internal handle for simple start() API - managed by the server itself.
  // When start() is called, the handle is stored here and the server takes ownership.
  // When startDetached() is called, the handle is returned to the caller.
  AsyncHandle _internalHandle;

  // Used by MultiHttpServer to track lifecycle without strong ownership.
  std::weak_ptr<ServerLifecycleTracker> _lifecycleTracker;

#ifdef AERONET_ENABLE_OPENSSL
  internal::TlsRuntimeState _tls;
#endif
};

}  // namespace aeronet
