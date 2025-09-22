#pragma once

#include <sys/uio.h>  // iovec

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "flat-hash-map.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"
#include "http-response-writer.hpp"
#include "raw-chars.hpp"
#include "timedef.hpp"

namespace aeronet {

class EventLoop;  // forward declaration

// HttpServer
//  - Single-threaded event loop by design: one instance == one epoll/reactor running in the
//    calling thread (typically the thread invoking run() / runUntil()).
//  - Not internally synchronized; do not access a given instance concurrently from multiple
//    threads (except destroying after stop()).
//  - To utilize multiple CPU cores, create several HttpServer instances (possibly with
//    ServerConfig::withReusePort(true) on the same port) and run each in its own thread. Or better, use the provided
//    MultiHttpServer class made for this purpose.
//  - Writes currently assume exclusive ownership of the connection fd within this single
//    thread, enabling simple sequential ::write / ::writev without partial-write state tracking.
class HttpServer {
 public:
  using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;
  using StreamingHandler = std::function<void(const HttpRequest&, HttpResponseWriter&)>;
  enum class ParserError : std::uint8_t {
    BadRequestLine,
    VersionUnsupported,
    HeadersTooLarge,
    PayloadTooLarge,
    MalformedChunk,
    GenericBadRequest
  };
  using ParserErrorCallback = std::function<void(ParserError)>;

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
  explicit HttpServer(const ServerConfig& cfg);

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&& other) noexcept;
  HttpServer& operator=(HttpServer&& other) noexcept;

  ~HttpServer();

  // Registers a single request handler that will be invoked for every successfully parsed
  // HTTP request. The handler receives a fully populated immutable HttpRequest reference and
  // must return an HttpResponse by value (moved out). The returned response is serialized and
  // queued for write immediately after the handler returns (unless a streaming handler is in
  // effect).
  //
  // Exclusivity / precedence:
  //   - Mutually exclusive with setStreamingHandler (only one request processing mode).
  //   - Mutually exclusive with addPathHandler / path based dispatch. Attempting to mix will
  //     throw std::logic_error. Choose either a global handler or per‑path handlers.
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
  // Exclusivity:
  //   - Mutually exclusive with setHandler and with any path handlers (addPathHandler). If a
  //     global or path handler is already registered this call throws.
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
  void setStreamingHandler(StreamingHandler handler);  // mutually exclusive with setHandler / path handlers (phase 1)
  // Register a handler for a specific absolute path and a set of allowed HTTP methods.
  // Methods are supplied via http::MethodsSet (small fixed-capacity flat set, non-allocating).
  // Mutually exclusive with setHandler: using both is invalid and will throw.
  void addPathHandler(std::string path, const http::MethodSet& methods, const RequestHandler& handler);

  // Convenience overload of 'addPathHandler' for a single method.
  void addPathHandler(std::string path, http::Method method, const RequestHandler& handler);

  // Install a callback invoked whenever the request parser encounters a non‑recoverable
  // protocol error for a connection. Typical causes correspond to the ParserError enum:
  //   * BadRequestLine        -> Malformed start line (method / target / version)
  //   * VersionUnsupported    -> HTTP version not supported
  //   * HeadersTooLarge       -> Cumulative header size exceeded configured limits
  //   * PayloadTooLarge       -> Declared (Content-Length) body size exceeds limits
  //   * MalformedChunk        -> Invalid chunk size line / trailer in chunked encoding
  //   * GenericBadRequest     -> Fallback category for other parse failures
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
  void run(Duration checkPeriod = std::chrono::milliseconds{500});

  // Run the server until the user-supplied predicate returns true (checked once per loop iteration) or stop() is
  // invoked / signal received. Semantics of checkPeriod are identical to run(): it is the upper bound on how long we
  // may block waiting for new events when idle. The predicate is evaluated after processing any ready events and
  // before the next epoll_wait call. A very small checkPeriod will evaluate the predicate more frequently, but at the
  // cost of additional wake‑ups when idle.
  void runUntil(const std::function<bool()>& predicate, Duration checkPeriod = std::chrono::milliseconds{500});

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
  // Threading considerations:
  //   - _running is a plain bool; cross‑thread visibility relies on typical compiler & platform
  //     behavior for simple flag polling. For rigorous cross‑thread memory ordering a future
  //     revision may make it std::atomic<bool>, but in practice the short checkPeriod bounds
  //     any delay.
  //
  // Typical usage:
  //   - From a signal handler wrapper (set a flag then call stop() in a safe context).
  //   - From a controller thread coordinating multiple HttpServer instances.
  void stop();

  // The config given to the server, with the actual allocated port if 0 was given.
  [[nodiscard]] const ServerConfig& config() const { return _config; }

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

 private:
  friend class HttpResponseWriter;  // allow streaming writer to access queueData and _connStates

  struct ConnStateInternal {
    RawChars buffer;         // accumulated raw data
    RawChars bodyStorage;    // decoded body lifetime
    RawChars outBuffer;      // pending outbound bytes not yet written
    RawChars decodedTarget;  // storage for percent-decoded request target (per-connection reuse)
    std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
    uint32_t requestsServed{0};
    bool shouldClose{false};      // request to close once outBuffer drains
    bool waitingWritable{false};  // EPOLLOUT registered
  };
  void eventLoop(Duration timeout);
  void refreshCachedDate();
  void sweepIdleConnections();
  void acceptNewConnections();
  void handleReadableClient(int fd);
  bool processRequestsOnConnection(int fd, ConnStateInternal& state);
  // Split helpers
  bool parseNextRequestFromBuffer(int fd, ConnStateInternal& state, HttpRequest& outReq, std::size_t& headerEnd,
                                  bool& closeConn);
  bool decodeBodyIfReady(int fd, ConnStateInternal& state, const HttpRequest& req, std::size_t headerEnd,
                         bool isChunked, bool expectContinue, bool& closeConn, std::size_t& consumedBytes);
  bool decodeFixedLengthBody(int fd, ConnStateInternal& state, const HttpRequest& req, std::size_t headerEnd,
                             bool expectContinue, bool& closeConn, std::size_t& consumedBytes);
  bool decodeChunkedBody(int fd, ConnStateInternal& state, const HttpRequest& req, std::size_t headerEnd,
                         bool expectContinue, bool& closeConn, std::size_t& consumedBytes);
  void finalizeAndSendResponse(int fd, ConnStateInternal& state, HttpRequest& req, HttpResponse& resp,
                               std::size_t consumedBytes, bool& closeConn);
  // Outbound write helpers
  bool queueData(int fd, ConnStateInternal& state, const char* data, std::size_t len);
  bool queueVec(int fd, ConnStateInternal& state, const struct iovec* iov, int iovcnt);
  void flushOutbound(int fd, ConnStateInternal& state);
  void handleWritableClient(int fd);
  void closeConnection(int fd);

  struct StatsInternal {
    uint64_t totalBytesQueued{0};
    uint64_t totalBytesWrittenImmediate{0};
    uint64_t totalBytesWrittenFlush{0};
    uint64_t deferredWriteEvents{0};
    uint64_t flushCycles{0};
    std::size_t maxConnectionOutboundBuffer{0};
  } _stats;

 public:
  struct StatsPublic {
    uint64_t totalBytesQueued;
    uint64_t totalBytesWrittenImmediate;
    uint64_t totalBytesWrittenFlush;
    uint64_t deferredWriteEvents;
    uint64_t flushCycles;
    std::size_t maxConnectionOutboundBuffer;
  };

  [[nodiscard]] StatsPublic stats() const {
    return {_stats.totalBytesQueued,
            _stats.totalBytesWrittenImmediate,
            _stats.totalBytesWrittenFlush,
            _stats.deferredWriteEvents,
            _stats.flushCycles,
            _stats.maxConnectionOutboundBuffer};
  }

  int _listenFd{-1};
  bool _running{false};
  RequestHandler _handler;
  StreamingHandler _streamingHandler;
  struct PathHandlerEntry {
    uint32_t methodMask;
    std::array<RequestHandler, http::kNbMethods> handlers;
  };

  flat_hash_map<std::string, PathHandlerEntry, std::hash<std::string_view>, std::equal_to<>> _pathHandlers;

  std::unique_ptr<EventLoop> _loop;
  ServerConfig _config;
  flat_hash_map<int, ConnStateInternal> _connStates;  // per-server connection states

  using RFC7231DateStr = std::array<char, 29>;

  RFC7231DateStr _cachedDate{};
  TimePoint _cachedDateEpoch;  // last second-aligned timestamp used for Date header
  ParserErrorCallback _parserErrCb;
};
}  // namespace aeronet
