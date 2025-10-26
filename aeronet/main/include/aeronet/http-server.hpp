#pragma once

#include <sys/eventfd.h>  // eventfd wakeups
#include <sys/uio.h>      // iovec

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string_view>

#include "accept-encoding-negotiation.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/lifecycle.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "connection-state.hpp"
#include "connection.hpp"
#include "event-loop.hpp"
#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/ssl.h>  // ensure real ::SSL is visible (avoid shadowing forward decl)

#include "tls-context.hpp"  // brings TlsContext & TlsMetricsExternal
#include "tls-metrics.hpp"
#endif

#include "aeronet/http-method.hpp"
#include "aeronet/server-stats.hpp"
#include "encoder.hpp"
#include "flat-hash-map.hpp"
#include "http-response-writer.hpp"
#include "socket.hpp"

namespace aeronet {

class ITransport;  // forward declaration for TLS/plain transport abstraction
#ifdef AERONET_ENABLE_OPENSSL
class TlsContext;  // forward declaration still okay
// NOTE: We intentionally do NOT forward declare SSL here because doing so inside the aeronet namespace would create
// a different type aeronet::SSL, shadowing the real ::SSL from OpenSSL and breaking overload resolution for
// SSL_* functions. Instead we include <openssl/ssl.h> above under the same feature flag, which provides the
// correct ::SSL definition while keeping this header lightweight when TLS support is disabled.
#endif

// HttpServer
//  - Single-threaded event loop by design: one instance == one epoll/reactor running in the
//    calling thread (typically the thread invoking run() / runUntil()).
//  - Not internally synchronized; do not access a given instance concurrently from multiple
//    threads (except destroying after stop()).
//  - To utilize multiple CPU cores, create several HttpServer instances (possibly with
//    HttpServerConfig::withReusePort(true) on the same port) and run each in its own thread. Or better, use the
//    provided MultiHttpServer class made for this purpose.
//  - Writes currently assume exclusive ownership of the connection fd within this single
//    thread, enabling simple sequential ::write / ::writev without partial-write state tracking.
class HttpServer {
 public:
  using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

  using StreamingHandler = std::function<void(const HttpRequest&, HttpResponseWriter&)>;

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

  // Construct a HttpServer that does nothing.
  // Useful only to make it default constructible for temporary purposes (for instance to move assign to it later on),
  // but do not attempt to use a default constructed server, it will not bind to any socket.
  HttpServer() noexcept = default;

  // Construct a server bound and listening immediately according to given configuration.
  //  - Performs: ::socket, setsockopt (REUSEADDR always, REUSEPORT best-effort if enabled), ::bind, ::listen,
  //    retrieves (and overwrites cfg.port with) the chosen ephemeral port if cfg.port == 0, sets O_NONBLOCK,
  //    and registers the listening fd with the internal EventLoop.
  //  - If any step fails it throws std::runtime_error (leaving no open fd).
  //  - After construction port() returns the actual bound port (deterministic for tests using ephemeral ports).
  explicit HttpServer(HttpServerConfig config, RouterConfig routerConfig = {});

  // Constructs a server bound and listening immediately according to given configuration,
  // and using the provided Router for request routing (can be configured after construction before run).
  HttpServer(HttpServerConfig cfg, Router router);

  // Move semantics & constraints:
  // -----------------------------
  // A HttpServer can be moved ONLY when it is not running. Attempting to move (construct or assign from) a
  // running server is a logic error: the event loop thread would continue executing against the old "this"
  // while ownership of its internal epoll fd, listening socket, wakeup fd, connection maps, handlers and TLS
  // context had been transferred, leading to immediate undefined behaviour. To make this failure mode explicit
  // and prevent silent partial moves, the move constructor and move assignment operator now THROW
  // std::runtime_error if the source object is running.
  //
  // Design choice:
  //  * We intentionally drop noexcept on move operations to surface misuse instead of asserting and then
  //    forcing a partially moved/stopped state.
  //  * This keeps normal (non‑running) moves available for ergonomic construction & storage patterns.
  //
  // Safe usage pattern:
  //    HttpServer tmp(cfg);
  //    tmp.router().setDefault(...);
  //    HttpServer server(std::move(tmp)); // OK (tmp not running)
  //    std::jthread t([&]{ server.run(); });
  //
  // Invalid usage (throws std::runtime_error):
  //    HttpServer s(cfg);
  //    std::jthread t([&]{ s.run(); });
  //    HttpServer moved(std::move(s)); // throws
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&& other);             // NOLINT(performance-noexcept-move-constructor)
  HttpServer& operator=(HttpServer&& other);  // NOLINT(performance-noexcept-move-constructor)

  ~HttpServer();

  // Get the object managing per-path handlers.
  // You may use it to modify path handlers after initial configuration.
  Router& router() noexcept { return _router; }

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
  void setMetricsCallback(MetricsCallback cb);

  // Register or clear the expectation handler. This handler will be invoked from
  // the server's event-loop thread when a request contains an `Expect` header
  // with tokens other than "100-continue". To clear, pass an empty std::function.
  void setExpectationHandler(ExpectationHandler handler);

  // Run the server event loop until stop() is called (e.g. from another thread) or the process receives SIGINT/SIGTERM.
  // Run the server event loop until stop() is called (from another thread) or SIGINT/SIGTERM.
  // The maximum blocking interval of a single poll cycle is controlled by HttpServerConfig::pollInterval.
  // This method is blocking for the caller thread.
  void run();

  // Run the server until the user-supplied predicate returns true (checked once per loop iteration) or stop() is
  // invoked / signal received. Semantics of checkPeriod are identical to run(): it is the upper bound on how long we
  // may block waiting for new events when idle. The predicate is evaluated after processing any ready events and
  // before the next epoll_wait call. A very small checkPeriod will evaluate the predicate more frequently, but at the
  // cost of additional wake‑ups when idle.
  // Like run() but exits when the user-supplied predicate returns true (checked once per loop iteration) or stop()
  // is invoked / signal received. Poll sleep upper bound is HttpServerConfig::pollInterval.
  void runUntil(const std::function<bool()>& predicate);

  // Requests cooperative termination of the event loop. Safe to invoke from a different thread
  // (best‑effort). The maximum observable latency before run()/runUntil() return is bounded by
  // the checkPeriod supplied to those functions (epoll returns earlier if events arrive).
  // New incoming connections are prevented by closing the listening socket immediately;
  // existing established connections are not force‑closed – they simply stop being serviced once the loop exits.
  // Usually called from a different thread than the one that started the server, this method is not blocking,
  // so the server might not be immediately stopped once the method returns to the caller.
  //
  // Idempotency:
  //   - Repeated calls are harmless. Calling after the server already stopped has no effect.
  //
  // Typical usage:
  //   - From a signal handler wrapper (set a flag then call stop() in a safe context).
  //   - From a controller thread coordinating multiple HttpServer instances.
  // Note: it is possible to call 'run()' again on a stopped server.
  void stop() noexcept;

  // Initiate graceful draining: stop accepting new connections, and close existing keep-alive
  // sessions after their current in-flight response completes. When maxWait > 0 a deadline is
  // enforced, after which remaining connections are closed immediately. Safe to call from a
  // different thread. Calling beginDrain() while already draining updates the deadline to the
  // earliest of the current and new values.
  void beginDrain(std::chrono::milliseconds maxWait = std::chrono::milliseconds{0}) noexcept;

  // The config given to the server, with the actual allocated port if 0 was given.
  // The config is immutable after creation of the Server.
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

  [[nodiscard]] bool isDraining() const { return _lifecycle.isDraining(); }

  [[nodiscard]] ServerStats stats() const;

 private:
  friend class HttpResponseWriter;  // allow streaming writer to access queueData and _connStates

  using ConnectionMap = flat_hash_map<Connection, ConnectionState, std::hash<int>, std::equal_to<>>;

  using ConnectionMapIt = ConnectionMap::iterator;

  void init();
  void prepareRun();

  void eventLoop();
  void sweepIdleConnections();
  void acceptNewConnections();
  void handleReadableClient(int fd);
  bool processRequestsOnConnection(ConnectionMapIt cnxIt);
  // Split helpers
  bool decodeBodyIfReady(ConnectionMapIt cnxIt, HttpRequest& req, bool isChunked, bool expectContinue,
                         std::size_t& consumedBytes);
  bool decodeFixedLengthBody(ConnectionMapIt cnxIt, HttpRequest& req, bool expectContinue, std::size_t& consumedBytes);
  bool decodeChunkedBody(ConnectionMapIt cnxIt, HttpRequest& req, bool expectContinue, std::size_t& consumedBytes);
  bool maybeDecompressRequestBody(ConnectionMapIt cnxIt, HttpRequest& req);
  void finalizeAndSendResponse(ConnectionMapIt cnxIt, const HttpRequest& req, HttpResponse&& resp,
                               std::size_t consumedBytes, std::chrono::steady_clock::time_point reqStart);
  // Handle Expect header tokens other than the built-in 100-continue.
  // Returns true if processing should stop for this request (response already queued/sent).
  bool handleExpectHeader(ConnectionMapIt cnxIt, HttpRequest& req, ConnectionState& state, bool& found100Continue,
                          std::chrono::steady_clock::time_point reqStart);
  // Helper to populate and invoke the metrics callback for a completed request.
  void emitRequestMetrics(const HttpRequest& req, http::StatusCode status, std::size_t bytesIn, bool reusedConnection,
                          std::chrono::steady_clock::time_point reqStart);
  // Helper to build & queue a simple error response, invoke parser error callback (if any).
  // If immediate=true the connection will be closed without waiting for buffered writes to drain.
  void emitSimpleError(ConnectionMapIt cnxIt, http::StatusCode code, bool immediate = false,
                       std::string_view reason = {});
  // Outbound write helpers
  bool queuePreparedResponse(ConnectionMapIt cnxIt, HttpResponse::PreparedResponse prepared);
  bool queueData(ConnectionMapIt cnxIt, HttpResponseData httpResponseData, std::uint64_t extraQueuedBytes = 0);
  void flushOutbound(ConnectionMapIt cnxIt);
  void flushFilePayload(ConnectionMapIt cnxIt);
  // Helper: attempt to flush any pending bytes in tunnelOrFileBuffer for a connection.
  // Returns true if the caller should return early because the buffer is still non-empty or write is pending.
  bool flushPendingTunnelOrFileBuffer(ConnectionMapIt cnxIt);

  void handleWritableClient(int fd);

  ConnectionMapIt closeConnection(ConnectionMapIt cnxIt);

  // Invoke a registered streaming handler. Returns true if the connection should be closed after handling
  // the request (either because the client requested it or keep-alive limits reached). The HttpRequest is
  // non-const because we may reuse shared response finalization paths (e.g. emitting a 406 early) that expect
  // to mutate transient fields (target normalization already complete at this point).
  bool callStreamingHandler(const StreamingHandler& streamingHandler, HttpRequest& req, ConnectionMapIt cnxIt,
                            std::size_t consumedBytes, std::chrono::steady_clock::time_point reqStart);

  enum class LoopAction : uint8_t { Nothing, Continue, Break };

  LoopAction processSpecialMethods(ConnectionMapIt& cnxIt, const HttpRequest& req, std::size_t consumedBytes,
                                   std::chrono::steady_clock::time_point reqStart);

  void handleInTunneling(ConnectionMapIt cnxIt);

  void closeListener() noexcept;
  void closeAllConnections(bool immediate);

  void registerBuiltInProbes();

  struct StatsInternal {
    uint64_t totalBytesQueued{0};
    uint64_t totalBytesWrittenImmediate{0};
    uint64_t totalBytesWrittenFlush{0};
    uint64_t deferredWriteEvents{0};
    uint64_t flushCycles{0};
    uint64_t epollModFailures{0};
    std::size_t maxConnectionOutboundBuffer{0};
  } _stats;

  // Helpers to enable/disable writable interest (EPOLLOUT) for a connection. They wrap
  // ModWithCloseOnFailure and update `ConnectionState::waitingWritable` and internal stats
  // consistently. Return true on success, false on failure (caller should handle close).
  bool enableWritableInterest(ConnectionMapIt cnxIt, const char* ctx);
  bool disableWritableInterest(ConnectionMapIt cnxIt, const char* ctx);

  HttpServerConfig _config;

  Socket _listenSocket;  // listening socket
  EventLoop _eventLoop;  // epoll-based event loop

  internal::Lifecycle _lifecycle;

  Router _router;

  ConnectionMap _connStates;

  // Pre-allocated encoders (one per supported format) constructed once at server creation.
  // Index corresponds to static_cast<size_t>(Encoding).
  std::array<std::unique_ptr<Encoder>, kNbContentEncodings> _encoders;
  EncodingSelector _encodingSelector;

  ParserErrorCallback _parserErrCb = []([[maybe_unused]] http::StatusCode) {};
  MetricsCallback _metricsCb;
  ExpectationHandler _expectationHandler;
  RawChars _tmpBuffer;  // can be used for any kind of temporary buffer

  // Telemetry context - one per HttpServer instance (no global singletons)
  tracing::TelemetryContext _telemetry;

#ifdef AERONET_ENABLE_OPENSSL
  // TlsContext lifetime & pointer stability:
  // ----------------------------------------
  // OpenSSL's SSL_CTX_set_alpn_select_cb stores the opaque `void* arg` pointer and later invokes the
  // callback during each TLS handshake. We pass a pointer to our TlsContext so the callback can access
  // ALPN configuration / metrics.
  //
  // If we placed TlsContext directly as a value member (or inside std::optional) and later moved the
  // encompassing HttpServer, the TlsContext object would be moved / reconstructed at a new address while
  // OpenSSL still retains the original pointer, leading to a dangling pointer and potential UAF during
  // future handshakes (we previously observed this as an ASan stack-use-after-return when a temporary was
  // moved into an optional after registration).
  //
  // Storing TlsContext behind a std::unique_ptr guarantees a stable object address for the entire
  // HttpServer lifetime irrespective of HttpServer moves; only the owning smart pointer value changes.
  // This is the least invasive way to provide pointer stability without prohibiting HttpServer move
  // semantics or introducing a heavier PImpl layer.
  std::unique_ptr<TlsContext> _tlsCtxHolder;  // stable address for OpenSSL callbacks
  TlsMetricsInternal _tlsMetrics;             // defined in tls-metrics.hpp
  // External metrics struct used by TLS context for ALPN mismatch increments only.
  TlsMetricsExternal _tlsMetricsExternal;  // shares alpnStrictMismatches with _tlsMetrics (synced in stats retrieval)
#endif
};

}  // namespace aeronet
