#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "compression-config.hpp"
#include "invalid_argument_exception.hpp"
#include "request-decompression-config.hpp"
#include "tls-config.hpp"

namespace aeronet {

struct HttpServerConfig {
  enum class TrailingSlashPolicy : std::int8_t { Strict, Normalize, Redirect };

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
  // Default: 1 MiB.
  std::size_t maxBodyBytes{1 << 20};  // 1 MiB

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

  // Behavior for resolving paths that differ only by a trailing slash.
  // Default: Normalize
  TrailingSlashPolicy trailingSlashPolicy{TrailingSlashPolicy::Normalize};

  // ===========================================
  // Response compression configuration
  // ===========================================
  // Attempt negotiation according to configured formats / thresholds.
  // Actual encoder availability also depends on build flags
  // (e.g. AERONET_ENABLE_ZLIB). Future: brotli, zstd guarded likewise.
  CompressionConfig compression;

  // ===========================================
  // Request body decompression configuration
  // ===========================================
  RequestDecompressionConfig requestDecompression;

  // ===========================================
  // Header merge behavior tuning
  // ===========================================
  // When merging repeated unknown (i.e. not in the curated table) request headers, the default policy (true)
  // assumes list semantics and joins with a comma. If set to false, unknown headers are treated as non-mergeable
  // (duplicates will be handled according to parser singleton logic or rejected). This allows stricter deployments
  // to avoid accidentally merging custom singleton semantics.
  bool mergeUnknownRequestHeaders{true};

  // Validates config. Throws invalid_argument if it is not valid.
  // TODO
  void validate() const {}

 private:
  TLSConfig& ensureTls() {
    if (!tls) {
      tls.emplace();
    }
    return *tls;
  }

 public:
  // Fluent builder style setters
  HttpServerConfig& withPort(uint16_t port) {  // Set explicit listening port (0 = ephemeral)
    this->port = port;
    return *this;
  }

  HttpServerConfig& withReusePort(bool on = true) {  // Enable/disable SO_REUSEPORT
    this->reusePort = on;
    return *this;
  }

  HttpServerConfig& withKeepAliveMode(bool on = true) {  // Toggle persistent connections
    this->enableKeepAlive = on;
    return *this;
  }

  HttpServerConfig& withMaxHeaderBytes(std::size_t maxHeaderBytes) {  // Adjust header size ceiling
    this->maxHeaderBytes = maxHeaderBytes;
    return *this;
  }

  HttpServerConfig& withMaxBodyBytes(std::size_t maxBodyBytes) {  // Adjust body size limit
    this->maxBodyBytes = maxBodyBytes;
    return *this;
  }

  HttpServerConfig& withMaxOutboundBufferBytes(std::size_t maxOutbound) {  // Adjust per-connection outbound queue cap
    this->maxOutboundBufferBytes = maxOutbound;
    return *this;
  }

  HttpServerConfig& withMaxRequestsPerConnection(uint32_t maxRequests) {  // Adjust request-per-connection cap
    this->maxRequestsPerConnection = maxRequests;
    return *this;
  }

  HttpServerConfig& withKeepAliveTimeout(std::chrono::milliseconds timeout) {  // Adjust idle keep-alive timeout
    this->keepAliveTimeout = timeout;
    return *this;
  }

  HttpServerConfig& withPollInterval(std::chrono::milliseconds interval) {  // Adjust event loop max idle wait
    this->pollInterval = interval;
    return *this;
  }

  HttpServerConfig& withHeaderReadTimeout(std::chrono::milliseconds timeout) {  // Set slow header read timeout (0=off)
    this->headerReadTimeout = timeout;
    return *this;
  }

  // Accept any string-like source (const char*, std::string, std::string_view) for certificate & key file paths.
  // We intentionally copy here because configuration happens once at startup; micro-optimizing moves is unnecessary.
  HttpServerConfig& withTlsCertKey(std::string_view certFile, std::string_view keyFile) {
    auto& tlsCfg = ensureTls();
    tlsCfg.certFile.assign(certFile);
    tlsCfg.keyFile.assign(keyFile);
    return *this;
  }

  HttpServerConfig& withTlsCipherList(std::string_view cipherList) {
    ensureTls().cipherList.assign(cipherList);
    return *this;
  }

  HttpServerConfig& withTlsMinVersion(std::string_view ver) {
    ensureTls().minVersion.assign(ver);
    return *this;
  }

  HttpServerConfig& withTlsMaxVersion(std::string_view ver) {
    ensureTls().maxVersion.assign(ver);
    return *this;
  }

  // Provide in-memory PEM certificate & key instead of file paths. Overwrites any previously set file-based values.
  HttpServerConfig& withTlsCertKeyMemory(std::string_view certPem, std::string_view keyPem) {
    auto& tlsCfg = ensureTls();
    tlsCfg.certFile.clear();
    tlsCfg.keyFile.clear();
    tlsCfg.certPem.assign(certPem);
    tlsCfg.keyPem.assign(keyPem);
    return *this;
  }

  HttpServerConfig& withTlsRequestClientCert(bool on = true) {
    ensureTls().requestClientCert = on;
    return *this;
  }

  // Enforce mutual TLS: handshake fails if client does not present *and* validate a certificate.
  HttpServerConfig& withTlsRequireClientCert(bool on = true) {
    auto& tlsCfg = ensureTls();
    tlsCfg.requireClientCert = on;
    if (on) {
      tlsCfg.requestClientCert = true;  // logical implication
    }
    return *this;
  }

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
  HttpServerConfig& withTlsAlpnMustMatch(bool on = true) {
    ensureTls().alpnMustMatch = on;
    return *this;
  }

  // Enable/disable verbose one-line handshake logging (ALPN, cipher suite, TLS version, peer subject if present)
  HttpServerConfig& withTlsHandshakeLogging(bool on = true) {
    ensureTls().logHandshake = on;
    return *this;
  }

  HttpServerConfig& withTlsHandshakeTimeout(std::chrono::milliseconds timeout) {
    tlsHandshakeTimeout = timeout;
    return *this;
  }

  // Add a single trusted client certificate (PEM) to verification store (useful for tests / pinning). Multiple allowed.
  HttpServerConfig& withTlsAddTrustedClientCert(std::string_view certPem) {
    ensureTls().trustedClientCertsPem.emplace_back(certPem);
    return *this;
  }

  HttpServerConfig& withoutTls() {
    tls.reset();
    return *this;
  }

  // Policy for handling a trailing slash difference between registered path handlers and incoming requests.
  // Resolution algorithm (independent of policy):
  //   1. ALWAYS attempt an exact match on the incoming target string first. If found, dispatch that handler.
  //      (This means if both "/p" and "/p/" are registered, each is honored exactly as requested; no policy logic
  //      runs.)
  //   2. If no exact match:
  //        a) If the request ends with one trailing slash (not root) and the canonical form without the slash exists:
  //             - Strict   : treat as not found (404).
  //             - Normalize: internally treat it as the canonical path (strip slash, no redirect).
  //             - Redirect : emit a 301 with Location header pointing to the canonical (no trailing slash) path.
  //        b) Else if the request does NOT end with a slash, policy is Normalize, and ONLY the slashed variant exists
  //             (e.g. "/x/" registered, "/x" not): treat the slashed variant as equivalent and dispatch to it.
  //        c) Otherwise: 404 (no transformation / redirect performed).
  //   3. Root path "/" is never redirected or normalized.
  //
  // Summary:
  //   Strict   : exact-only matching; variants differ; no implicit mapping.
  //   Normalize: provide symmetric acceptance (one missing variant maps to the existing one) without redirects.
  //   Redirect : like Strict unless the ONLY difference is an added trailing slash for a canonical registered path;
  //              then a 301 to the canonical form is sent (never the inverse).
  HttpServerConfig& withTrailingSlashPolicy(TrailingSlashPolicy policy) {
    trailingSlashPolicy = policy;
    return *this;
  }

  // Enable / configure response compression. Passing by value allows caller to move.
  HttpServerConfig& withCompression(CompressionConfig cfg) {
    compression = std::move(cfg);
    return *this;
  }

  HttpServerConfig& withRequestDecompression(RequestDecompressionConfig cfg) {
    requestDecompression = std::move(cfg);
    return *this;
  }

  HttpServerConfig& withMergeUnknownRequestHeaders(bool on = true) {
    mergeUnknownRequestHeaders = on;
    return *this;
  }
};

static_assert(std::is_aggregate_v<HttpServerConfig>,
              "HttpServerConfig should be an aggregate to be used conveniently by the client");

}  // namespace aeronet
