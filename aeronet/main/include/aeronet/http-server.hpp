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
#include <string>
#include <string_view>
#include <type_traits>

#include "accept-encoding-negotiation.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "connection-state.hpp"
#include "connection.hpp"
#include "event-fd.hpp"
#include "event-loop.hpp"
#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/ssl.h>  // ensure real ::SSL is visible (avoid shadowing forward decl)

#include "tls-context.hpp"  // brings TlsContext & TlsMetricsExternal
#include "tls-metrics.hpp"
#endif

#include "aeronet/http-method-set.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/server-stats.hpp"
#include "encoder.hpp"
#include "flat-hash-map.hpp"
#include "http-response-writer.hpp"
#include "socket.hpp"
#include "timedef.hpp"

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
    http::Method method;
    std::string_view path;
    int status{0};
    uint64_t bytesIn{0};
    uint64_t bytesOut{0};
    std::chrono::nanoseconds duration{0};
    bool reusedConnection{false};
  };

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
  explicit HttpServer(HttpServerConfig cfg);

  // Move semantics & constraints:
  // -----------------------------
  // HttpServer is movable only while it is NOT running (i.e. before the first call to run()/runUntil(), or
  // after those calls have returned). Moving a running server would transfer internal epoll state, connection
  // maps and the TLS context out from under the active event‑loop thread causing immediate undefined behavior
  // (use‑after‑move on the old object's members). This is inherently unsafe and therefore disallowed.
  //
  // Enforced policy:
  //  * If a move (construction or assignment) observes other._running == true we log an error and fire an assert
  //    to indicate the client that it is invoking undefined behavior.
  //
  // Rationale for allowing (stopped) moves:
  //  * Tests and higher‑level wrappers occasionally want to construct into a temporary then store into a container
  //    or aggregate without an extra heap indirection; supporting move in the quiescent state keeps this ergonomic.
  //
  // Safe usage pattern:
  //    HttpServer tmp(cfg);
  //    tmp.setHandler(...);
  //    tmp.setXXXX(...);
  //    HttpServer server(std::move(tmp)); // OK (tmp not running)
  //    std::jthread t([&]{ server.runUntil(stopFlag); });
  //
  // Unsafe pattern:
  //    HttpServer s(cfg);
  //    std::jthread t([&]{ s.run(); });
  //    HttpServer moved(std::move(s)); // FATAL: moving while running.
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&& other) noexcept;
  HttpServer& operator=(HttpServer&& other) noexcept;

  ~HttpServer();

  // Registers a single request handler that will be invoked for every successfully parsed
  // HTTP request not matched by a path‑specific handler (normal or streaming). The handler
  // receives a fully populated immutable HttpRequest reference and must return an HttpResponse
  // by value (moved out). The returned response is serialized and queued for write immediately
  // after the handler returns.
  //
  // Precedence (Phase 2 mixing model):
  //   1. Path streaming handler (if registered for path+method)
  //   2. Path normal handler (if registered for path+method)
  //   3. Global streaming handler (if set)
  //   4. Global normal handler (this)
  //   5. 404 / 405 fallback
  //
  // Mixing:
  //   - Global normal and streaming handlers may both be set; per‑path handlers override them.
  //   - Replacing a global handler is allowed at any time (not thread‑safe; caller must ensure exclusive access).
  //
  // Timing & threading:
  //   - The handler executes synchronously inside the server's single event loop thread; do
  //     not perform blocking operations of long duration inside it (offload to another thread
  //     if needed and respond later via a queued response mechanism – future enhancement).
  //   - Because only one event loop thread exists per server instance, no additional
  //     synchronization is required for data local to the handler closure, but you must still
  //     synchronize access to state shared across multiple server instances.
  //
  // Lifetime:
  //   - You may call setHandler() before or after run()/runUntil(); replacing the handler
  //     while the server is processing requests is safe (the new handler will be used for
  //     subsequent requests) but avoid doing so concurrently from another thread.
  //
  // Error handling:
  //   - Exceptions escaping the handler will be caught, converted to a 500 response, and the
  //     connection may be closed depending on the internal policy (implementation detail).
  //
  // Performance notes:
  //   - Returning large payloads benefits from move semantics; construct HttpResponse in place
  //     and return; small-string optimizations usually avoid allocations for short headers.
  void setHandler(RequestHandler handler);

  // Enables incremental / chunked style responses using HttpResponseWriter instead of returning
  // a fully materialized HttpResponse object. Intended for large / dynamic payloads (server‑sent
  // data, on‑the‑fly generation) or when you wish to start sending bytes before the complete
  // body is available.
  //
  // Mixing (Phase 2):
  //   - May coexist with a global normal handler and with per‑path (normal or streaming) handlers.
  //   - Acts only as a fallback when no path‑specific handler matches.
  //
  // Invocation semantics:
  //   - The streaming handler runs synchronously inside the event loop thread after a request
  //     has been fully parsed (headers + body by current design). Future evolution may allow
  //     body streaming; today you receive the complete request body.
  //   - For HEAD requests the writer is constructed in a mode that suppresses body emission; you
  //     may still call write() but payload bytes are discarded while headers are sent.
  //
  // Writer contract:
  //   - You may set status / headers up until the first write(). If you never explicitly set a
  //     Content-Length the response is transferred using chunked encoding (unless HTTP/1.0).
  //   - Call writer.end() to finalize the response. If you return without calling end(), the
  //     server will automatically end() for you (sending last chunk / final CRLF) unless a fatal
  //     error occurred.
  //   - write() applies simple backpressure by queuing into the connection's outbound buffer; a
  //     false return indicates a fatal condition (connection closing / overflow) – cease writing.
  //
  // Keep‑alive & connection reuse:
  //   - After the handler returns the server evaluates standard keep‑alive rules (HTTP/1.1,
  //     config.enableKeepAlive, request count < maxRequestsPerConnection, no close flag). If any
  //     condition fails the connection is marked to close once buffered bytes flush.
  //
  // Performance & blocking guidance:
  //   - Avoid long blocking operations; they stall the entire server instance. Offload heavy
  //     work to a different thread and stream results back if necessary (future async hooks may
  //     simplify this pattern).
  //
  // Exceptions:
  //   - Exceptions thrown by the handler are caught and logged; the server attempts to end the
  //     response gracefully (typically as already started chunked stream). Subsequent writes are
  //     ignored once a failure state is reached.
  void setStreamingHandler(StreamingHandler handler);
  // Register a handler for a specific absolute path and a set of allowed HTTP methods.
  // Methods are supplied via http::MethodsSet (small fixed-capacity flat set, non-allocating).
  // May coexist with global handlers and with per-path streaming handlers (but a specific
  // (path, method) pair cannot have both a normal and streaming handler simultaneously).
  void addPathHandler(std::string path, const http::MethodSet& methods, const RequestHandler& handler);

  // Convenience overload of 'addPathHandler' for a single method.
  void addPathHandler(std::string path, http::Method method, const RequestHandler& handler);

  // addPathStreamingHandler (multi-method):
  //   Registers streaming handlers per path+method combination. Mirrors addPathHandler semantics
  //   but installs a StreamingHandler which receives an HttpResponseWriter.
  // Constraints:
  //   - For each (path, method) only one of normal vs streaming may be present; registering the
  //     other kind afterwards is a logic error.
  // Overwrite semantics:
  //   - Re-registering the same kind (streaming over streaming) replaces the previous handler.
  void addPathStreamingHandler(std::string path, const http::MethodSet& methods, const StreamingHandler& handler);

  // addPathStreamingHandler (single method convenience):
  void addPathStreamingHandler(std::string path, http::Method method, const StreamingHandler& handler);

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
  void setParserErrorCallback(ParserErrorCallback cb) { _parserErrCb = std::move(cb); }
  void setMetricsCallback(MetricsCallback cb) { _metricsCb = std::move(cb); }

  // Run the server event loop until stop() is called (e.g. from another thread) or the process receives SIGINT/SIGTERM.
  // checkPeriod:
  // Acts as the maximum sleep / blocking interval in the internal poll loop (passed as the timeout to epoll_wait).
  // Lower values:
  //   + Faster responsiveness to external stop() calls.
  //   + Finer granularity for periodic housekeeping (idle connection sweeping, cached Date header refresh).
  //   - More wake‑ups -> higher baseline CPU usage.
  // Higher values:
  //   + Fewer wake‑ups (reduced idle CPU) when the server is mostly idle.
  //   - May delay detection of: (a) stop() requests, (b) keep‑alive timeout expiry, (c) Date header second rollover
  //     by up to the specified duration.
  // Epoll will still return early when I/O events arrive, so this is only a cap on maximum latency when *idle*.
  // Typical practical ranges:
  //   - High throughput / low latency tuning:   5–50 ms
  //   - General purpose / balanced default:     50–250 ms
  //   - Extremely low churn / power sensitive:  250–1000 ms (at the cost of slower shutdown & timeout precision)
  // Default (500 ms) favors low idle CPU over sub‑100 ms shutdown responsiveness.
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
  // (best‑effort); the next loop iteration observes _running == false and exits. The maximum
  // observable latency before run()/runUntil() return is bounded by the checkPeriod supplied to
  // those functions (epoll returns earlier if events arrive). New incoming connections are
  // prevented by closing the listening socket immediately; existing established connections are
  // not force‑closed – they simply stop being serviced once the loop exits.
  //
  // Idempotency:
  //   - Repeated calls are harmless. Calling after the server already stopped has no effect.
  //
  // Typical usage:
  //   - From a signal handler wrapper (set a flag then call stop() in a safe context).
  //   - From a controller thread coordinating multiple HttpServer instances.
  void stop();

  // The config given to the server, with the actual allocated port if 0 was given.
  // The config is immutable after creation of the Server.
  [[nodiscard]] const HttpServerConfig& config() const { return _config; }

  // Get the actual port of this server.
  // If the configuration port was 0, the port has been automatically allocated by the system.
  [[nodiscard]] uint16_t port() const { return _config.port; }

  // Returns true while the event loop is actively executing inside run() / runUntil(), and
  // false otherwise (before start, after stop(), or after loop exit due to predicate / error).
  // Because _running is a plain bool and may be toggled from another thread via stop(), a
  // concurrent observer may see a short delay (bounded by checkPeriod) before the flag turns
  // false. Primarily intended for coarse-grained coordination / diagnostics, not for
  // high-precision synchronization. For deterministic shutdown sequencing prefer joining the
  // thread that called run()/runUntil().
  [[nodiscard]] bool isRunning() const { return _running; }

  [[nodiscard]] ServerStats stats() const;

 private:
  friend class HttpResponseWriter;  // allow streaming writer to access queueData and _connStates

  void eventLoop(Duration timeout);
  void refreshCachedDate();
  void sweepIdleConnections();
  void acceptNewConnections();
  void handleReadableClient(int fd);
  bool processRequestsOnConnection(int fd, ConnectionState& state);
  // Split helpers
  std::size_t parseNextRequestFromBuffer(int fd, ConnectionState& state, HttpRequest& outReq);
  bool decodeBodyIfReady(int fd, ConnectionState& state, HttpRequest& req, bool isChunked, bool expectContinue,
                         std::size_t& consumedBytes);
  bool decodeFixedLengthBody(int fd, ConnectionState& state, HttpRequest& req, bool expectContinue,
                             std::size_t& consumedBytes);
  bool decodeChunkedBody(int fd, ConnectionState& state, HttpRequest& req, bool expectContinue,
                         std::size_t& consumedBytes);
  bool maybeDecompressRequestBody(int fd, ConnectionState& state, HttpRequest& req);
  void finalizeAndSendResponse(int fd, ConnectionState& state, HttpRequest& req, HttpResponse& resp,
                               std::size_t consumedBytes, std::chrono::steady_clock::time_point reqStart);
  // Helper to build & queue a simple error response, invoke parser error callback (if any).
  // If immediate=true the connection will be closed without waiting for buffered writes to drain.
  void emitSimpleError(int fd, ConnectionState& state, http::StatusCode code, bool immediate = false);
  // Outbound write helpers
  bool queueData(int fd, ConnectionState& state, std::string_view data);
  void flushOutbound(int fd, ConnectionState& state);

  void handleWritableClient(int fd);

  void closeConnectionFd(int fd);

  // Invoke a registered streaming handler. Returns true if the connection should be closed after handling
  // the request (either because the client requested it or keep-alive limits reached). The HttpRequest is
  // non-const because we may reuse shared response finalization paths (e.g. emitting a 406 early) that expect
  // to mutate transient fields (target normalization already complete at this point).
  bool callStreamingHandler(const StreamingHandler& streamingHandler, HttpRequest& req, int fd, ConnectionState& state,
                            std::size_t consumedBytes, std::chrono::steady_clock::time_point reqStart);

  // Transport-aware helpers (fall back to raw fd if transport null)
  static ssize_t transportRead(int fd, ConnectionState& state, std::size_t chunkSize, bool& wantRead, bool& wantWrite);
  static ssize_t transportWrite(int fd, ConnectionState& state, std::string_view data, bool& wantRead, bool& wantWrite);

  struct StatsInternal {
    uint64_t totalBytesQueued{0};
    uint64_t totalBytesWrittenImmediate{0};
    uint64_t totalBytesWrittenFlush{0};
    uint64_t deferredWriteEvents{0};
    uint64_t flushCycles{0};
    uint64_t epollModFailures{0};
    std::size_t maxConnectionOutboundBuffer{0};
    uint64_t streamingChunkCoalesced{0};
    uint64_t streamingChunkLarge{0};
  } _stats;

  // Attempt an epoll_ctl MOD on the given fd; on failure logs, marks connection for close and
  // increments failure metric. Returns true on success, false on failure.
  // EBADF / ENOENT (race where fd already closed / removed) are logged at WARN (not ERROR).
  static bool ModWithCloseOnFailure(EventLoop& loop, int fd, uint32_t events, ConnectionState& st, const char* ctx,
                                    StatsInternal& stats);

  Socket _listenSocket;  // listening socket RAII
  // Wakeup fd (eventfd) used to interrupt epoll_wait promptly when stop() is invoked from another thread.
  EventFd _wakeupFd;
  bool _running{false};
  RequestHandler _handler;
  StreamingHandler _streamingHandler;
  struct PathHandlerEntry {
    uint32_t normalMethodMask{};
    uint32_t streamingMethodMask{};
    std::array<RequestHandler, http::kNbMethods> normalHandlers{};
    std::array<StreamingHandler, http::kNbMethods> streamingHandlers{};
  };

  flat_hash_map<std::string, PathHandlerEntry, std::hash<std::string_view>, std::equal_to<>> _pathHandlers;

  EventLoop _loop;
  HttpServerConfig _config;

  flat_hash_map<Connection, ConnectionState, std::hash<int>, std::equal_to<>> _connStates;

  // Pre-allocated encoders (one per supported format) constructed once at server creation.
  // Index corresponds to static_cast<size_t>(Encoding).
  std::array<std::unique_ptr<Encoder>, kNbContentEncodings> _encoders;
  EncodingSelector _encodingSelector;

  using RFC7231DateStr = std::array<char, 29>;

  RFC7231DateStr _cachedDate{};
  TimePoint _cachedDateEpoch;  // last second-aligned timestamp used for Date header
  ParserErrorCallback _parserErrCb = []([[maybe_unused]] http::StatusCode) {};
  MetricsCallback _metricsCb;
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

static_assert(std::is_nothrow_default_constructible_v<HttpServer>);

}  // namespace aeronet
