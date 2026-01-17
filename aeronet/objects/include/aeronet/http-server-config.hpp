#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/builtin-probes-config.hpp"
#include "aeronet/concatenated-headers.hpp"
#include "aeronet/concatenated-strings.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/telemetry-config.hpp"
#include "compression-config.hpp"
#include "decompression-config.hpp"
#include "tls-config.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-config.hpp"
#endif

namespace aeronet {

struct HttpServerConfig {
  // ============================
  // Threading parameters
  // ============================
  // Number of threads to use for the server event loops.
  // This setting is only meaningful for HttpServer (aka MultiHttpServer);
  // SingleHttpServer always uses one thread, and will throw in case nbThreads > 1.
  // If 0, the server will use std::thread::hardware_concurrency() for MultiHttpServer,
  // or 1 for SingleHttpServer.
  // Default: 0.
  uint32_t nbThreads{0};

  // ============================
  // Listener / socket parameters
  // ============================
  // TCP port to bind. 0 (default) lets the OS pick an ephemeral free port. After construction
  // you can retrieve the effective port via SingleHttpServer::port().
  uint16_t port{0};

  // If true, enables SO_REUSEPORT allowing multiple independent server instances to bind the same port
  // for load distribution by the kernel.
  // Note that for MultiHttpServer, if false it will check port availability only if given port is not ephemeral
  // and if the server is multi-threaded at construction time. Indeed, by design, MultiHttpServer will force reuse port
  // for all its internal servers when threadCount > 1. Default: false.
  bool reusePort{false};

  // TCP_NODELAY is a socket option that disables the Nagle algorithm, a TCP congestion control technique that buffers
  // small packets to combine them into a single larger segment. By setting TCP_NODELAY, applications can force the
  // kernel to send data immediately, reducing latency in applications that require low latency and send small packets
  // frequently, such as interactive services or request-response protocols. This favors lower latency over network
  // efficiency, which is good for some use cases but can lead to more network overhead if overused. Default: false.
  bool tcpNoDelay{false};

  // ===========================================
  // Keep-Alive / connection lifecycle controls
  // ===========================================

  // Whether HTTP/1.1 persistent connections (keep-alive) are enabled. When false, server always closes after
  // each response regardless of client headers. Default: true.
  bool enableKeepAlive{true};

  // ===========================================
  // Header merge behavior tuning
  // ===========================================
  // When merging repeated unknown (i.e. not in the curated table) request headers, the default policy (true)
  // assumes list semantics and joins with a comma. If set to false, unknown headers are treated as non-mergeable
  // (duplicates will be handled according to parser singleton logic or rejected). This allows stricter deployments
  // to avoid accidentally merging custom singleton semantics.
  bool mergeUnknownRequestHeaders{true};

  enum class TraceMethodPolicy : std::uint8_t {
    Disabled,
    // Allow on both plaintext and TLS
    EnabledPlainAndTLS,
    // Allow only on plaintext connections (disable on TLS)
    EnabledPlainOnly
  };

  // Enable TRACE method handling (echo) on the server. Disabled by default for safety.
  TraceMethodPolicy traceMethodPolicy{TraceMethodPolicy::Disabled};

  // Maximum number of HTTP requests to serve over a single persistent connection before forcing close.
  // A high value improves connection reuse at the cost of potential resource exhaustion from slow clients.
  // A low value limits resource usage but may increase latency due to more frequent connection establishment.
  // Default: 100,000 requests.
  uint32_t maxRequestsPerConnection{100000};

  // Maximum number of closed ConnectionState objects to cache for reuse to reduce allocations.
  // When the limit is reached, closed connections are fully destroyed. Default: 100.
  uint32_t maxCachedConnections{100};

  // Idle timeout for keep-alive connections (duration to wait for next request after previous response is fully
  // sent). Once exceeded the server proactively closes the connection.
  // Default: 5000 ms.
  std::chrono::milliseconds keepAliveTimeout{std::chrono::milliseconds{5000}};

  // ============================
  // Request parsing & body limits
  // ============================
  // Maximum allowed size (in bytes) of the aggregate HTTP request head (request line + all headers + CRLFCRLF).
  // If exceeded while parsing, the server replies 431/400 and closes the connection. Default: 8 KiB.
  std::size_t maxHeaderBytes{8192};

  // Maximum allowed size (in bytes) of a request body (after decoding any chunked framing). Requests exceeding
  // this limit result in a 413 (Payload Too Large) HTTP error and closure.
  // Default: 256 MiB.
  std::size_t maxBodyBytes{1 << 28};  // 256 MiB

  // For requests with a captured body, HttpResponse will concatenate the captured body contents with the head in the
  // same buffer if their size is below this threshold. This can be efficient for small bodies because it
  // improves cache locality and will probably save one system socket call. Larger bodies will be kept separate.
  // Default: 1 KiB.
  std::size_t minCapturedBodySize{1024};  // 1 KiB

  // =============================================
  // Outbound buffering & backpressure management
  // =============================================
  // Upper bound (bytes) for data queued but not yet written to the client socket for a single connection.
  // Includes headers + body (streaming or aggregated). When exceeded further writes are rejected and the
  // connection marked for closure after flushing what is already queued. Default: 4 MiB per connection.
  std::size_t maxOutboundBufferBytes{4 << 20};  // 4 MiB

  // ===========================================
  // Event loop polling / responsiveness tuning
  // ===========================================
  // Maximum duration the event loop will block waiting for I/O in a single epoll_wait() when idle before it wakes to
  // check for external stop conditions (stop() call or runUntil predicate).
  //
  // Note: periodic timeout enforcement (keep-alive idle timeout, header/body read timeouts, TLS handshake timeout)
  // is driven by an internal timerfd registered in epoll. This decouples timeout enforcement from whether
  // epoll_wait() happens to hit its timeout under load.
  // Lower values:
  //   + Faster responsiveness to external stop() calls.
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
  std::chrono::milliseconds pollInterval{std::chrono::milliseconds{500}};

  // ===========================================
  // Slowloris / header read timeout mitigation
  // ===========================================
  // Maximum duration allowed to fully receive the HTTP request headers (request line + headers + CRLFCRLF)
  // from the moment the first byte of the request is read on a connection. If exceeded before the header
  // terminator is observed the server closes the connection and emits a 408 Request Timeout. A value
  // of 0 disables this protective timeout. Default: disabled.
  std::chrono::milliseconds headerReadTimeout{std::chrono::milliseconds{0}};

  // Maximum time spent waiting for additional body bytes once a handler blocks on HttpRequest::body() or
  // HttpRequest::readBody(). Applies after headers are parsed and guards against slowloris-style uploads.
  // The timer resets whenever progress is made (bytes consumed). A value of 0 disables the timeout.
  std::chrono::milliseconds bodyReadTimeout{std::chrono::milliseconds{0}};

  // =================
  // TLS configuration
  // =================
  TLSConfig tls;

#ifdef AERONET_ENABLE_HTTP2
  // ===========================================
  // HTTP/2 protocol configuration
  // ===========================================
  /// HTTP/2 settings including frame sizes, flow control, and feature toggles.
  /// Use `http2.enable` to control whether HTTP/2 is globally enabled.
  /// When enabled and a client supports HTTP/2:
  /// - TLS connections negotiating "h2" via ALPN will use HTTP/2
  /// - Cleartext connections may upgrade via h2c (if http2.enableH2c is true)
  /// The same handlers (RequestHandler, StreamingHandler, AsyncRequestHandler)
  /// work transparently for both HTTP/1.1 and HTTP/2.
  Http2Config http2;
#endif

  // Telemetry configuration (OpenTelemetry tracing + DogStatsD metrics)
  TelemetryConfig telemetry;

  // ===========================================
  // Response compression configuration
  // ===========================================
  // Attempt negotiation according to configured formats / thresholds.
  // Actual encoder availability also depends on build flags
  // (e.g. AERONET_ENABLE_ZLIB).
  CompressionConfig compression;

  // ===========================================
  // Request body decompression configuration
  // ===========================================
  DecompressionConfig decompression;

  // ===========================================
  // Adaptive inbound read chunk sizing
  // ===========================================
  // The server initially used a fixed 4096 byte read() size for all inbound socket reads. To better balance
  // header latency and bulk body throughput we expose a two‑tier strategy:
  //   * minReadChunkBytes     : the minimum transport read size chunk used while parsing request data.
  //                             The buffer grows exponentially as needed up to this size while parsing headers.
  //   * maxPerEventReadBytes  : optional fairness cap. 0 => unlimited (loop continues until EAGAIN / short read).
  //                             When >0 the server stops reading from a connection once this many bytes were
  //                             successfully read in the current epoll event, yielding back to the event loop.
  // Defaults chosen to preserve prior behavior (4K) unless body phase tuning is explicitly desired.
  std::uint32_t minReadChunkBytes{4096};
  std::uint32_t maxPerEventReadBytes{0};

  // Hard limit to avoid pathological cases with excessive global headers, which would bloat response size
  // and waste CPU serializing them.
  static constexpr uint32_t kMaxGlobalHeaders = 256;

  // Will add all the headers defined here in all server responses, if not explicitly set by the user for a given
  // response. Defaults to a list of one entry "server: aeronet".
  // Each added header MUST have their key and value separated by (exactly one) aeronet::http::HeaderSep.
  // The maximum number of global headers is kMaxGlobalHeaders.
  ConcatenatedHeaders globalHeaders{{"server: aeronet"}};

  // ===========================================
  // Builtin Kubernetes-style probes configuration
  // ===========================================
  BuiltinProbesConfig builtinProbes;

  // Validates and (possibly) finalize config. Throws std::invalid_argument if it is not valid.
  void validate();

 private:
  TLSConfig& ensureTls();

  ConcatenatedStrings32 _connectAllowlist;

 public:
  // Optional allowlist for CONNECT targets (hostnames or IP string). When empty, CONNECT to any
  // resolved host is allowed. When non-empty, the target host must exactly match one of these entries. The matching is
  // case-insensitive for hostnames.
  [[nodiscard]] const ConcatenatedStrings32& connectAllowlist() const { return _connectAllowlist; }

  // Set number of threads to use for the server event loops.
  // This setting is only meaningful for HttpServer (aka MultiHttpServer);
  HttpServerConfig& withNbThreads(uint32_t nbThreads);

  // Set explicit listening port (0 = ephemeral)
  HttpServerConfig& withPort(uint16_t port);

  // Enable/disable SO_REUSEPORT
  HttpServerConfig& withReusePort(bool on = true);

  // Toggle persistent connections
  HttpServerConfig& withKeepAliveMode(bool on = true);

  // Toggle TCP_NODELAY (disable the Nagle algorithm). Default: false.
  HttpServerConfig& withTcpNoDelay(bool on = true);

  // Adjust header size ceiling
  HttpServerConfig& withMaxHeaderBytes(std::size_t maxHeaderBytes);

  // Adjust body size limit
  HttpServerConfig& withMaxBodyBytes(std::size_t maxBodyBytes);

  // Adjust threshold (bytes) under which captured body contents are appended inline with the head.
  HttpServerConfig& withMinCapturedBodySize(std::size_t bytes);

  // Adjust per-connection outbound queue cap
  HttpServerConfig& withMaxOutboundBufferBytes(std::size_t maxOutbound);

  // Adjust request-per-connection cap
  HttpServerConfig& withMaxRequestsPerConnection(uint32_t maxRequests);

  // Adjust idle keep-alive timeout
  HttpServerConfig& withKeepAliveTimeout(std::chrono::milliseconds timeout);

  // Maximum number of closed connection objects to cache for reuse to reduce allocations.
  HttpServerConfig& withMaxCachedConnections(uint32_t maxCachedConnections);

  // Adjust event loop max idle wait
  HttpServerConfig& withPollInterval(std::chrono::milliseconds interval);

  // Set slow header read timeout (0=off)
  HttpServerConfig& withHeaderReadTimeout(std::chrono::milliseconds timeout);

  // Set blocking body read timeout (0=off)
  HttpServerConfig& withBodyReadTimeout(std::chrono::milliseconds timeout);

  // Optional allowlist for CONNECT targets (hostnames or IP string). When empty, CONNECT to any
  // resolved host is allowed. When non-empty, the target host must exactly match one of these entries.
  // The matching is case-insensitive for hostnames.
  // Set CONNECT allowlist (replaces any existing entries). An empty list allows all targets.
  template <class InputIt>
  HttpServerConfig& withConnectAllowlist(InputIt first, InputIt last) {
    _connectAllowlist.clear();
    for (auto it = first; it != last; ++it) {
      _connectAllowlist.append(*it);
    }
    return *this;
  }

  HttpServerConfig& withTlsCertKey(std::string_view certFile, std::string_view keyFile);

  HttpServerConfig& withTlsCipherList(std::string_view cipherList);

  HttpServerConfig& withTlsMinVersion(std::string_view ver);

  HttpServerConfig& withTlsMaxVersion(std::string_view ver);

  // Provide in-memory PEM certificate & key instead of file paths. Overwrites any previously set file-based values.
  HttpServerConfig& withTlsCertKeyMemory(std::string_view certPem, std::string_view keyPem);

  HttpServerConfig& withTlsRequestClientCert(bool on = true);

  // Enforce mutual TLS: handshake fails if client does not present *and* validate a certificate.
  HttpServerConfig& withTlsRequireClientCert(bool on = true);

  HttpServerConfig& withTlsKtlsMode(TLSConfig::KtlsMode mode);

  // Set (overwrite) ALPN protocol preference list. Order matters; first matching protocol is selected.
  template <std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_reference_t<R>, std::string_view>
  HttpServerConfig& withTlsAlpnProtocols(R&& protos) {
    auto& tlsCfg = ensureTls();
    tlsCfg.withTlsAlpnProtocols(std::forward<R>(protos));
    return *this;
  }

  // Set (overwrite) ALPN protocol preference list. Order matters; first matching protocol is selected.
  template <class InputIt>
  HttpServerConfig& withTlsAlpnProtocols(InputIt first, InputIt last) {
    return withTlsAlpnProtocols(std::ranges::subrange(first, last));
  }

  // Set (overwrite) ALPN protocol preference list. Order matters; first matching protocol is selected.
  template <typename T>
    requires std::convertible_to<T, std::string_view>
  HttpServerConfig& withTlsAlpnProtocols(std::initializer_list<T> protos) {
    return withTlsAlpnProtocols(std::ranges::subrange(protos.begin(), protos.end()));
  }

  // Require ALPN negotiation success (handshake aborts if client and server share no protocol).
  HttpServerConfig& withTlsAlpnMustMatch(bool on = true);

  // Enable/disable verbose one-line handshake logging (ALPN, cipher suite, TLS version, peer subject if present)
  HttpServerConfig& withTlsHandshakeLogging(bool on = true);

  HttpServerConfig& withTlsHandshakeTimeout(std::chrono::milliseconds timeout);

  // Add a single trusted client certificate (PEM) to verification store (useful for tests / pinning). Multiple allowed.
  HttpServerConfig& withTlsTrustedClientCert(std::string_view certPem);

  // Disable TLS entirely
  HttpServerConfig& withoutTls();

  // Enable / configure response compression. Passing by value allows caller to move.
  HttpServerConfig& withCompression(CompressionConfig cfg);

  HttpServerConfig& withRequestDecompression(DecompressionConfig cfg);

  HttpServerConfig& withMergeUnknownRequestHeaders(bool on = true);

  // Set the telemetry configuration for this server instance
  HttpServerConfig& withTelemetryConfig(TelemetryConfig cfg);

  // Configure the minimum read chunk size when receiving request data. Returns *this.
  HttpServerConfig& withMinReadChunkBytes(std::size_t minReadChunkBytes);

  // Configure a per-event read fairness cap (0 => unlimited)
  HttpServerConfig& withMaxPerEventReadBytes(std::size_t capBytes);

  // Replace the global response headers list
  HttpServerConfig& withGlobalHeaders(std::span<const http::Header> headers);

  // Convenience: add a single global header entry (appended)
  HttpServerConfig& addGlobalHeader(const http::Header& header);

  // Set TRACE handling policy. Default: Disabled.
  HttpServerConfig& withTracePolicy(TraceMethodPolicy policy);

  // Enable and configure builtin probes
  HttpServerConfig& withBuiltinProbes(BuiltinProbesConfig cfg);

  // Enable (or disable) builtin probes with default configuration.
  HttpServerConfig& enableBuiltinProbes(bool on = true);

#ifdef AERONET_ENABLE_HTTP2
  // Configure HTTP/2 protocol settings
  HttpServerConfig& withHttp2(Http2Config cfg);

  // Enable or disable HTTP/2 globally (shorthand for http2.enable = on)
  HttpServerConfig& enableHttp2(bool on = true);
#endif
};

}  // namespace aeronet
