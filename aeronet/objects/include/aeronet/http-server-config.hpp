#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/http-header.hpp"
#include "aeronet/otel-config.hpp"
#include "aeronet/router-config.hpp"
#include "compression-config.hpp"
#include "decompression-config.hpp"
#include "invalid_argument_exception.hpp"
#include "tls-config.hpp"

namespace aeronet {

struct HttpServerConfig {
  // RFC 7301 (ALPN) protocol identifier length is encoded in a single octet => maximum 255 bytes.
  // OpenSSL lacks a stable public constant for this; we define it here to avoid magic numbers.
  static constexpr std::size_t kMaxAlpnProtocolLength = 255;

  // ============================
  // Listener / socket parameters
  // ============================
  // TCP port to bind. 0 (default) lets the OS pick an ephemeral free port. After construction
  // you can retrieve the effective port via HttpServer::port().
  uint16_t port{0};

  // If true, enables SO_REUSEPORT allowing multiple independent HttpServer instances (usually one per thread)
  // to bind the same (non-ephemeral) port for load distribution by the kernel. Harmless if the platform
  // or kernel does not support it (failure is logged, not fatal). Disabled by default.
  bool reusePort{false};

  // ============================
  // Request parsing & body limits
  // ============================
  // Maximum allowed size (in bytes) of the aggregate HTTP request head (request line + all headers + CRLFCRLF).
  // If exceeded while parsing, the server replies 431/400 and closes the connection. Default: 8 KiB.
  std::size_t maxHeaderBytes{8192};

  // Maximum allowed size (in bytes) of a request body (after decoding any chunked framing). Requests exceeding
  // this limit result in a 413 (Payload Too Large) style error (currently 400/413 depending on path) and closure.
  // Default: 256 MiB.
  std::size_t maxBodyBytes{1 << 28};  // 256 MiB

  // For requests with a captured body, HttpResponse will concatenate the captured body contents with the head in the
  // same buffer if their size is below this threshold. This can be efficient for small bodies because it
  // improves cache locality and will probably save one system socket call. Larger bodies will be kept separate.
  // Default: 8 KiB.
  std::size_t minCapturedBodySize{8192};  // 8 KiB

  // =============================================
  // Outbound buffering & backpressure management
  // =============================================
  // Upper bound (bytes) for data queued but not yet written to the client socket for a single connection.
  // Includes headers + body (streaming or aggregated). When exceeded further writes are rejected and the
  // connection marked for closure after flushing what is already queued. Default: 4 MiB per connection.
  std::size_t maxOutboundBufferBytes{4 << 20};  // 4 MiB

  // ===========================================
  // Keep-Alive / connection lifecycle controls
  // ===========================================
  // Maximum number of HTTP requests to serve over a single persistent connection before forcing close.

  // OpenTelemetry configuration
  OtelConfig otel;
  // Helps cap memory use for long-lived clients and provides fairness. Default: 100.
  uint32_t maxRequestsPerConnection{100};

  // Whether HTTP/1.1 persistent connections (keep-alive) are enabled. When false, server always closes after
  // each response regardless of client headers. Default: true.
  bool enableKeepAlive{true};

  // Idle timeout for keep-alive connections (duration to wait for next request after previous response is fully
  // sent). Once exceeded the server proactively closes the connection. Default: 5000 ms.
  std::chrono::milliseconds keepAliveTimeout{std::chrono::milliseconds{5000}};

  // ===========================================
  // Event loop polling / responsiveness tuning
  // ===========================================
  // Maximum duration the event loop will block waiting for I/O in a single epoll_wait() when idle before it wakes to
  // perform housekeeping (idle sweep, Date header refresh) and to check for external stop conditions (stop() call or
  // runUntil predicate). Lower values -> faster shutdown / predicate reactivity but higher baseline wakeups. Higher
  // values -> lower idle CPU but slower responsiveness (bounded by this interval). Former run()/runUntil checkPeriod
  // parameter is now centralized here for configuration-at-construct-time consistency.
  std::chrono::milliseconds pollInterval{std::chrono::milliseconds{500}};  // formerly run() checkPeriod

  // ===========================================
  // Slowloris / header read timeout mitigation
  // ===========================================
  // Maximum duration allowed to fully receive the HTTP request headers (request line + headers + CRLFCRLF)
  // from the moment the first byte of the request is read on a connection. If exceeded before the header
  // terminator is observed the server closes the connection (optionally could emit 408 in future). A value
  // of 0 disables this protective timeout. Default: disabled.
  std::chrono::milliseconds headerReadTimeout{std::chrono::milliseconds{0}};

  // ===========================================
  // Optional TLS configuration
  // ===========================================
  // Presence (has_value) means user requests TLS; constructor will throw if OpenSSL support is not compiled in.
  std::optional<TLSConfig> tls;  // empty => plaintext

  // Protective timeout for TLS handshakes (time from accept to handshake completion). 0 => disabled.
  std::chrono::milliseconds tlsHandshakeTimeout{std::chrono::milliseconds{0}};

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
  DecompressionConfig requestDecompression;

  // ===========================================
  // Router configuration
  // ===========================================
  RouterConfig router;

  // ===========================================
  // Header merge behavior tuning
  // ===========================================
  // When merging repeated unknown (i.e. not in the curated table) request headers, the default policy (true)
  // assumes list semantics and joins with a comma. If set to false, unknown headers are treated as non-mergeable
  // (duplicates will be handled according to parser singleton logic or rejected). This allows stricter deployments
  // to avoid accidentally merging custom singleton semantics.
  bool mergeUnknownRequestHeaders{true};

  // ===========================================
  // Adaptive inbound read chunk sizing
  // ===========================================
  // The server initially used a fixed 4096 byte read() size for all inbound socket reads. To better balance
  // header latency and bulk body throughput we expose a two‑tier strategy:
  //   * initialReadChunkBytes : used while parsing the current request headers (request line + headers) until a full
  //                             head is parsed. Smaller keeps per-connection latency fair under high concurrency.
  //   * bodyReadChunkBytes    : used once headers are complete (while aggregating the body or after a full request); a
  //                             larger value improves throughput for large uploads. Applies also between requests
  //                             until the next header read begins (heuristic based on partial head detection).
  //   * maxPerEventReadBytes  : optional fairness cap. 0 => unlimited (loop continues until EAGAIN / short read).
  //                             When >0 the server stops reading from a connection once this many bytes were
  //                             successfully read in the current epoll event, yielding back to the event loop.
  // Defaults chosen to preserve prior behavior (4K) unless body phase tuning is explicitly desired.
  std::size_t initialReadChunkBytes{4096};
  std::size_t bodyReadChunkBytes{8192};
  std::size_t maxPerEventReadBytes{0};

  // Will add all the headers defined here in all server responses, when not present.
  // If the client sets a header present in this list, the client value will be kept.
  // Defaults to a list of one entry "Server: aeronet"
  std::vector<http::Header> globalHeaders{{"Server", "aeronet"}};

  // Enable TRACE method handling (echo) on the server. Disabled by default for safety.
  enum class TracePolicy : std::uint8_t {
    Disabled,
    // Allow on both plaintext and TLS
    EnabledPlainAndTLS,
    // Allow only on plaintext connections (disable on TLS)
    EnabledPlainOnly
  };
  TracePolicy tracePolicy{TracePolicy::Disabled};

  // Validates config. Throws invalid_argument if it is not valid.
  void validate() const;

 private:
  TLSConfig& ensureTls();

 public:
  // Set explicit listening port (0 = ephemeral)
  HttpServerConfig& withPort(uint16_t port);

  // Enable/disable SO_REUSEPORT
  HttpServerConfig& withReusePort(bool on = true);

  // Toggle persistent connections
  HttpServerConfig& withKeepAliveMode(bool on = true);

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

  // Adjust event loop max idle wait
  HttpServerConfig& withPollInterval(std::chrono::milliseconds interval);

  // Set slow header read timeout (0=off)
  HttpServerConfig& withHeaderReadTimeout(std::chrono::milliseconds timeout);

  // Accept any string-like source (const char*, std::string, std::string_view) for certificate & key file paths.
  // We intentionally copy here because configuration happens once at startup; micro-optimizing moves is unnecessary.
  HttpServerConfig& withTlsCertKey(std::string_view certFile, std::string_view keyFile);

  HttpServerConfig& withTlsCipherList(std::string_view cipherList);

  HttpServerConfig& withTlsMinVersion(std::string_view ver);

  HttpServerConfig& withTlsMaxVersion(std::string_view ver);

  // Provide in-memory PEM certificate & key instead of file paths. Overwrites any previously set file-based values.
  HttpServerConfig& withTlsCertKeyMemory(std::string_view certPem, std::string_view keyPem);

  HttpServerConfig& withTlsRequestClientCert(bool on = true);

  // Enforce mutual TLS: handshake fails if client does not present *and* validate a certificate.
  HttpServerConfig& withTlsRequireClientCert(bool on = true);

  // Set (overwrite) ALPN protocol preference list. Order matters; first matching protocol is selected.
  template <std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_reference_t<R>, std::string_view>
  HttpServerConfig& withTlsAlpnProtocols(R&& protos) {
    auto& tlsCfg = ensureTls();
    tlsCfg.alpnProtocols.clear();
    if constexpr (std::ranges::sized_range<R>) {
      tlsCfg.alpnProtocols.reserve(std::ranges::size(protos));
    }
    for (auto&& proto : protos) {
      // Ensure each element is materialized as a string (copy). This isolates lifetime concerns
      // when the input range is a temporary (e.g. braced initializer list or rvalue container).
      std::string_view sv(proto);
      if (sv.empty()) {
        throw invalid_argument("ALPN protocol entries must be non-empty");
      }
      if (std::cmp_greater(sv.size(), kMaxAlpnProtocolLength)) {
        throw invalid_argument("ALPN protocol entry length exceeds max {} bytes", kMaxAlpnProtocolLength);
      }
      tlsCfg.alpnProtocols.emplace_back(sv);
    }
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
  HttpServerConfig& withTlsAddTrustedClientCert(std::string_view certPem);

  HttpServerConfig& withoutTls();

  // Enable / configure response compression. Passing by value allows caller to move.
  HttpServerConfig& withCompression(CompressionConfig cfg);

  HttpServerConfig& withRequestDecompression(DecompressionConfig cfg);

  HttpServerConfig& withMergeUnknownRequestHeaders(bool on = true);

  // Set the OpenTelemetry configuration for this server instance
  HttpServerConfig& withOtelConfig(OtelConfig cfg) {
    otel = std::move(cfg);
    return *this;
  }

  // Configure adaptive read chunk sizing (two tier). Returns *this.
  HttpServerConfig& withReadChunkStrategy(std::size_t initialBytes, std::size_t bodyBytes) {
    initialReadChunkBytes = initialBytes;
    bodyReadChunkBytes = bodyBytes;
    return *this;
  }

  // Configure a per-event read fairness cap (0 => unlimited)
  HttpServerConfig& withMaxPerEventReadBytes(std::size_t capBytes) {
    maxPerEventReadBytes = capBytes;
    return *this;
  }

  // Replace the router configuration wholesale
  HttpServerConfig& withRouterConfig(RouterConfig cfg);

  // Replace the global response headers list
  HttpServerConfig& withGlobalHeaders(std::vector<http::Header> headers);

  // Convenience: add a single global header entry (appended)
  HttpServerConfig& withGlobalHeader(http::Header header);

  // Set TRACE handling policy. Default: Disabled.
  HttpServerConfig& withTracePolicy(TracePolicy policy);
};

}  // namespace aeronet
