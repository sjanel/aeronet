#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "invalid_argument_exception.hpp"
#include "tls-config.hpp"

namespace aeronet {

// Include for std::invalid_argument
struct ServerConfig {
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

 private:
  TLSConfig& ensureTls() {
    if (!tls) {
      tls.emplace();
    }
    return *tls;
  }

 public:
  // Fluent builder style setters
  ServerConfig& withPort(uint16_t port) {  // Set explicit listening port (0 = ephemeral)
    this->port = port;
    return *this;
  }

  ServerConfig& withReusePort(bool on = true) {  // Enable/disable SO_REUSEPORT
    this->reusePort = on;
    return *this;
  }

  ServerConfig& withKeepAliveMode(bool on = true) {  // Toggle persistent connections
    this->enableKeepAlive = on;
    return *this;
  }

  ServerConfig& withMaxHeaderBytes(std::size_t maxHeaderBytes) {  // Adjust header size ceiling
    this->maxHeaderBytes = maxHeaderBytes;
    return *this;
  }

  ServerConfig& withMaxBodyBytes(std::size_t maxBodyBytes) {  // Adjust body size limit
    this->maxBodyBytes = maxBodyBytes;
    return *this;
  }

  ServerConfig& withMaxOutboundBufferBytes(std::size_t maxOutbound) {  // Adjust per-connection outbound queue cap
    this->maxOutboundBufferBytes = maxOutbound;
    return *this;
  }

  ServerConfig& withMaxRequestsPerConnection(uint32_t maxRequests) {  // Adjust request-per-connection cap
    this->maxRequestsPerConnection = maxRequests;
    return *this;
  }

  ServerConfig& withKeepAliveTimeout(std::chrono::milliseconds timeout) {  // Adjust idle keep-alive timeout
    this->keepAliveTimeout = timeout;
    return *this;
  }

  ServerConfig& withHeaderReadTimeout(std::chrono::milliseconds timeout) {  // Set slow header read timeout (0=off)
    this->headerReadTimeout = timeout;
    return *this;
  }

  // Accept any string-like source (const char*, std::string, std::string_view) for certificate & key file paths.
  // We intentionally copy here because configuration happens once at startup; micro-optimizing moves is unnecessary.
  ServerConfig& withTlsCertKey(std::string_view certFile, std::string_view keyFile) {
    auto& tlsCfg = ensureTls();
    tlsCfg.certFile.assign(certFile);
    tlsCfg.keyFile.assign(keyFile);
    return *this;
  }

  ServerConfig& withTlsCipherList(std::string_view cipherList) {
    ensureTls().cipherList.assign(cipherList);
    return *this;
  }

  ServerConfig& withTlsMinVersion(std::string_view ver) {
    ensureTls().minVersion.assign(ver);
    return *this;
  }

  ServerConfig& withTlsMaxVersion(std::string_view ver) {
    ensureTls().maxVersion.assign(ver);
    return *this;
  }

  // Provide in-memory PEM certificate & key instead of file paths. Overwrites any previously set file-based values.
  ServerConfig& withTlsCertKeyMemory(std::string_view certPem, std::string_view keyPem) {
    auto& tlsCfg = ensureTls();
    tlsCfg.certFile.clear();
    tlsCfg.keyFile.clear();
    tlsCfg.certPem.assign(certPem);
    tlsCfg.keyPem.assign(keyPem);
    return *this;
  }

  ServerConfig& withTlsRequestClientCert(bool on = true) {
    ensureTls().requestClientCert = on;
    return *this;
  }

  // Enforce mutual TLS: handshake fails if client does not present *and* validate a certificate.
  ServerConfig& withTlsRequireClientCert(bool on = true) {
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
  ServerConfig& withTlsAlpnProtocols(R&& protos) {
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
  ServerConfig& withTlsAlpnProtocols(InputIt first, InputIt last) {
    return withTlsAlpnProtocols(std::ranges::subrange(first, last));
  }

  // Set (overwrite) ALPN protocol preference list. Order matters; first matching protocol is selected.
  template <typename T>
    requires std::convertible_to<T, std::string_view>
  ServerConfig& withTlsAlpnProtocols(std::initializer_list<T> protos) {
    return withTlsAlpnProtocols(std::ranges::subrange(protos.begin(), protos.end()));
  }

  // Require ALPN negotiation success (handshake aborts if client and server share no protocol).
  ServerConfig& withTlsAlpnMustMatch(bool on = true) {
    ensureTls().alpnMustMatch = on;
    return *this;
  }

  // Enable/disable verbose one-line handshake logging (ALPN, cipher suite, TLS version, peer subject if present)
  ServerConfig& withTlsHandshakeLogging(bool on = true) {
    ensureTls().logHandshake = on;
    return *this;
  }

  ServerConfig& withTlsHandshakeTimeout(std::chrono::milliseconds timeout) {
    tlsHandshakeTimeout = timeout;
    return *this;
  }

  // Add a single trusted client certificate (PEM) to verification store (useful for tests / pinning). Multiple allowed.
  ServerConfig& withTlsAddTrustedClientCert(std::string_view certPem) {
    ensureTls().trustedClientCertsPem.emplace_back(certPem);
    return *this;
  }

  ServerConfig& withoutTls() {
    tls.reset();
    return *this;
  }
};

static_assert(std::is_aggregate_v<ServerConfig>,
              "ServerConfig should be an aggregate to be used conveniently by the client");

}  // namespace aeronet
