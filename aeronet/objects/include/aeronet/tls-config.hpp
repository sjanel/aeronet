#pragma once

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/major-minor-version.hpp"
#include "aeronet/static-concatenated-strings.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

enum class TlsRevocationStatus : std::uint8_t { NoOpinion, Good, Revoked };

// View passed to an optional application revocation callback. nativeCertificate is an OpenSSL X509* exposed as an
// opaque pointer to keep OpenSSL headers out of the public configuration interface. It is valid only during callback.
struct TlsPeerCertificateView {
  const void* nativeCertificate;
  int chainDepth;
};

using TlsRevocationCallback = TlsRevocationStatus (*)(TlsPeerCertificateView certificate, void* userContext) noexcept;

class TLSConfig {
 public:
  using StringViewRange = std::ranges::subrange<ConcatenatedStrings32::iterator>;

  // RFC 7301 (ALPN) protocol identifier length is encoded in a single octet => maximum 255 bytes.
  // OpenSSL lacks a stable public constant for this; we define it here to avoid magic numbers.
  static constexpr std::size_t kMaxAlpnProtocolLength = 255;
  static constexpr std::size_t kSessionTicketKeySize = 48;

  static constexpr char kTlsVersionPrefix[] = "TLS";

  using Version = MajorMinorVersion<kTlsVersionPrefix>;

  enum class KtlsMode : std::uint8_t {
    Disabled,       // Never use kTLS
    Opportunistic,  // Use kTLS if available (DEFAULT)
    Enabled,        // Same as Opportunistic, but log a warning if kTLS not active
    Required,       // Fail connection if kTLS not active
  };
  enum class CipherPolicy : std::uint8_t { Default, Modern, Compatibility, Legacy };

  using SessionTicketKey = std::array<std::byte, kSessionTicketKeySize>;

  static constexpr Version TLS_1_2 = Version{1, 2};
  static constexpr Version TLS_1_3 = Version{1, 3};

  struct SniCertificate {
    SniCertificate() = default;

    [[nodiscard]] std::string_view pattern() const noexcept { return _strings[kPattern]; }
    void setPattern(std::string_view value) { _strings.set(kPattern, value); }

    [[nodiscard]] std::string_view certFile() const noexcept { return _strings[kCertFile]; }
    [[nodiscard]] auto certFileCstr() const noexcept { return _strings.c_str(kCertFile); }
    void setCertFile(std::string_view value) { _strings.set(kCertFile, value); }

    [[nodiscard]] std::string_view keyFile() const noexcept { return _strings[kKeyFile]; }
    [[nodiscard]] auto keyFileCstr() const noexcept { return _strings.c_str(kKeyFile); }
    void setKeyFile(std::string_view value) { _strings.set(kKeyFile, value); }

    [[nodiscard]] std::string_view certPem() const noexcept { return _strings[kCertPem]; }
    void setCertPem(std::string_view value) { _strings.set(kCertPem, value); }

    [[nodiscard]] std::string_view keyPem() const noexcept { return _strings[kKeyPem]; }
    void setKeyPem(std::string_view value) { _strings.set(kKeyPem, value); }

    [[nodiscard]] std::string_view ocspResponseFile() const noexcept { return _strings[kOcspResponseFile]; }
    [[nodiscard]] const char* ocspResponseFileCstr() const noexcept { return _strings.c_str(kOcspResponseFile); }
    void setOcspResponseFile(std::string_view value) { _strings.set(kOcspResponseFile, value); }

    [[nodiscard]] bool hasFiles() const noexcept { return !certFile().empty() || !keyFile().empty(); }
    [[nodiscard]] bool hasPem() const noexcept { return !certPem().empty() || !keyPem().empty(); }

    bool operator==(const SniCertificate&) const noexcept = default;

    bool isWildcard{false};

   private:
    enum : uint8_t {
      kPattern,
      kCertFile,
      kKeyFile,
      kCertPem,
      kKeyPem,
      kOcspResponseFile,
      kNbStrings,
    };

    // Secure: scrubs the private key when it is replaced, copied over or destroyed.
    StaticConcatenatedStrings<kNbStrings, uint32_t, true> _strings;
  };

  // The private keys and session ticket keys are scrubbed by their storage.

  void validate();

  // PEM server certificate file: the leaf certificate, then its intermediate certificates (sent to the clients)
  [[nodiscard]] std::string_view certFile() const noexcept { return _tlsStrings[kCertFile]; }
  [[nodiscard]] const char* certFileCstr() const noexcept { return _tlsStrings.c_str(kCertFile); }

  // PEM private key
  [[nodiscard]] std::string_view keyFile() const noexcept { return _tlsStrings[kKeyFile]; }
  [[nodiscard]] const char* keyFileCstr() const noexcept { return _tlsStrings.c_str(kKeyFile); }

  // In-memory PEM certificate, with its intermediate certificates (exclusive with certFile / keyFile)
  [[nodiscard]] std::string_view certPem() const noexcept { return _tlsStrings[kCertPem]; }
  [[nodiscard]] const char* certPemCstr() const noexcept { return _tlsStrings.c_str(kCertPem); }

  // In-memory PEM private key (exclusive with certFile / keyFile)
  [[nodiscard]] std::string_view keyPem() const noexcept { return _tlsStrings[kKeyPem]; }
  [[nodiscard]] const char* keyPemCstr() const noexcept { return _tlsStrings.c_str(kKeyPem); }

  // Optional OpenSSL cipher list string for TLS 1.2 (empty -> default). Exclusive with a non-default cipherPolicy.
  [[nodiscard]] std::string_view cipherList() const noexcept { return _tlsStrings[kCipherList]; }
  [[nodiscard]] const char* cipherListCstr() const noexcept { return _tlsStrings.c_str(kCipherList); }

  // Pre-fetched DER OCSP response. The file is parsed and cached when the TLS context is built; no network fetch is
  // performed during a handshake.
  [[nodiscard]] std::string_view ocspResponseFile() const noexcept { return _tlsStrings[kOcspResponseFile]; }
  [[nodiscard]] const char* ocspResponseFileCstr() const noexcept { return _tlsStrings.c_str(kOcspResponseFile); }

  // PEM or DER CRL used when verifying inbound client certificates.
  [[nodiscard]] std::string_view crlFile() const noexcept { return _tlsStrings[kCrlFile]; }
  [[nodiscard]] const char* crlFileCstr() const noexcept { return _tlsStrings.c_str(kCrlFile); }

  // NSS SSLKEYLOGFILE-compatible output path. Accepted only in builds without NDEBUG.
  [[nodiscard]] std::string_view keyLogFile() const noexcept { return _tlsStrings[kKeyLogFile]; }
  [[nodiscard]] const char* keyLogFileCstr() const noexcept { return _tlsStrings.c_str(kKeyLogFile); }

  TLSConfig& withTlsHandshakeTimeout(std::chrono::milliseconds timeout) {
    handshakeTimeout = timeout;
    return *this;
  }

  TLSConfig& withCertFile(std::string_view certFile) {
    _tlsStrings.set(kCertFile, certFile);
    return *this;
  }

  TLSConfig& withKeyFile(std::string_view keyFile) {
    _tlsStrings.set(kKeyFile, keyFile);
    return *this;
  }

  TLSConfig& withCertPem(std::string_view certPem) {
    _tlsStrings.set(kCertPem, certPem);
    return *this;
  }

  TLSConfig& withKeyPem(std::string_view keyPem) {
    _tlsStrings.set(kKeyPem, keyPem);
    return *this;
  }

  TLSConfig& withCipherList(std::string_view cipherList) {
    _tlsStrings.set(kCipherList, cipherList);
    return *this;
  }

  TLSConfig& withTlsCipherPolicy(CipherPolicy policy) {
    cipherPolicy = policy;
    return *this;
  }

  TLSConfig& withTlsOcspStapleFile(std::string_view derFile) {
    _tlsStrings.set(kOcspResponseFile, derFile);
    return *this;
  }

  TLSConfig& withoutTlsOcspStaple() {
    _tlsStrings.set(kOcspResponseFile, {});
    return *this;
  }

  TLSConfig& withTlsCrlFile(std::string_view file, bool checkAll = false) {
    _tlsStrings.set(kCrlFile, file);
    crlCheckAll = checkAll;
    return *this;
  }

  TLSConfig& withoutTlsCrl() {
    _tlsStrings.set(kCrlFile, {});
    crlCheckAll = false;
    return *this;
  }

  TLSConfig& withTlsRevocationCallback(TlsRevocationCallback callback, void* userContext = nullptr) {
    revocationCallback = callback;
    revocationUserContext = userContext;
    return *this;
  }

  TLSConfig& withTlsKeyLogFile(std::string_view file) {
    _tlsStrings.set(kKeyLogFile, file);
    return *this;
  }

  TLSConfig& withTlsMinVersion(std::string_view ver);

  TLSConfig& withTlsMaxVersion(std::string_view ver);

  TLSConfig& withTlsDisableCompression(bool disable = true) {
    disableCompression = disable;
    return *this;
  }

  TLSConfig& withTlsSessionTickets(bool on = true) {
    sessionTickets.enabled = on;
    return *this;
  }

  TLSConfig& withTlsSessionTicketLifetime(std::chrono::seconds lifetime) {
    sessionTickets.lifetime = lifetime;
    return *this;
  }

  TLSConfig& withTlsSessionTicketMaxKeys(std::uint32_t slots) {
    sessionTickets.maxKeys = slots;
    return *this;
  }

  TLSConfig& withTlsSessionTicketKey(SessionTicketKey keyMaterial);

  TLSConfig& clearTlsSessionTicketKeys() noexcept;

  TLSConfig& withTlsSniCertificateFiles(std::string_view hostname, std::string_view certPath, std::string_view keyPath);

  TLSConfig& withTlsSniCertificateMemory(std::string_view hostname, std::string_view certPem, std::string_view keyPem);

  // Associate a pre-fetched DER OCSP response with an existing exact or wildcard SNI certificate mapping.
  TLSConfig& withTlsSniOcspStapleFile(std::string_view hostname, std::string_view derFile);

  TLSConfig& clearTlsSniCertificates();

  // Set (overwrite) ALPN protocol preference list. Order matters; first matching protocol is selected.
  template <std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_reference_t<R>, std::string_view>
  TLSConfig& withTlsAlpnProtocols(R&& protos) {
    _alpnProtocols.clear();
    for (auto&& proto : protos) {
      _alpnProtocols.append(std::string_view(proto));
    }
    return *this;
  }

  // Append a trusted client certificate (PEM) to the list used for client cert validation.
  TLSConfig& withTlsTrustedClientCert(std::string_view certPem) {
    _trustedClientCertsPem.append(certPem);
    return *this;
  }

  TLSConfig& withKtlsMode(KtlsMode mode) {
    ktlsMode = mode;
    return *this;
  }

  TLSConfig& withTlsHandshakeConcurrencyLimit(std::uint32_t maxConcurrent) {
    maxConcurrentHandshakes = maxConcurrent;
    return *this;
  }

  TLSConfig& withTlsHandshakeRateLimit(std::uint32_t perSecond, std::uint32_t burst) {
    handshakeRateLimitPerSecond = perSecond;
    handshakeRateLimitBurst = burst;
    return *this;
  }

  // Clear all trusted client certificates.
  TLSConfig& withoutTlsTrustedClientCert() {
    _trustedClientCertsPem.clear();
    return *this;
  }

  // Ordered ALPN protocol list (first match preferred). Empty = disabled.
  [[nodiscard]] StringViewRange alpnProtocols() const noexcept {
    return {_alpnProtocols.begin(), _alpnProtocols.end()};
  }

  // Ordered ALPN protocol list (first match preferred). Empty = disabled.
  [[nodiscard]] StringViewRange trustedClientCertsPem() const noexcept {
    return {_trustedClientCertsPem.begin(), _trustedClientCertsPem.end()};
  }

  struct SessionTicketsConfig {
    bool operator==(const SessionTicketsConfig&) const noexcept = default;

    bool enabled{false};
    std::uint32_t maxKeys{2};
    std::chrono::seconds lifetime{std::chrono::hours{24}};
  } sessionTickets;

  // Protective timeout for TLS handshakes (time from accept to handshake completion). 0 => disabled.
  // Bounds the time a client can keep a handshake (and its connection) busy, even by trickling bytes.
  std::chrono::milliseconds handshakeTimeout{std::chrono::seconds{10}};

  TlsRevocationCallback revocationCallback{nullptr};
  void* revocationUserContext{nullptr};

  bool enabled{false};            // Master TLS enable/disable switch
  bool requestClientCert{false};  // Request (but not require) a client certificate
  // Require + verify client certificate (strict mTLS). Implies requestClientCert: validate() forces
  // requestClientCert=true whenever this is set, so enabling require always asks for the certificate.
  bool requireClientCert{false};
  bool alpnMustMatch{false};  // If true and client offers no overlapping ALPN protocol, fail handshake.
  bool logHandshake{false};  // If true, emit info log line on TLS handshake completion (ALPN, cipher, version, peer CN)
  bool disableCompression{true};  // Disable TLS-level compression (CRIME mitigation)
  bool crlCheckAll{false};        // Require CRLs for the full verified client chain instead of the leaf only.
  CipherPolicy cipherPolicy{CipherPolicy::Default};

  KtlsMode ktlsMode{KtlsMode::Opportunistic};

  // Optional protocol version bounds (empty => library defaults). Accepted values: "TLS1.2", "TLS1.3".
  Version minVersion;  // If set, enforce minimum TLS protocol version.
  Version maxVersion;  // If set, enforce maximum TLS protocol version.

  // Maximum number of concurrent TLS handshakes allowed. 0 => unlimited.
  std::uint32_t maxConcurrentHandshakes{0};

  // TLS handshake rate limiting (to mitigate DoS attacks)
  std::uint32_t handshakeRateLimitPerSecond{0};

  // Maximum burst size for handshake rate limiting
  std::uint32_t handshakeRateLimitBurst{0};

  bool operator==(const TLSConfig&) const noexcept = default;

 private:
  enum : uint8_t {
    kCertFile,
    kKeyFile,
    kCertPem,
    kKeyPem,
    kCipherList,
    kOcspResponseFile,
    kCrlFile,
    kKeyLogFile,
    kNbStrings,
  };

  // Static session ticket keys, scrubbed before their memory is released or reused (clear, assignment, destruction).
  struct StaticTicketKeys {
    StaticTicketKeys() noexcept = default;

    StaticTicketKeys(const StaticTicketKeys&) = default;
    StaticTicketKeys(StaticTicketKeys&&) noexcept = default;

    StaticTicketKeys& operator=(const StaticTicketKeys& other) {
      if (this != &other) {
        StaticTicketKeys copy(other);
        scrub();
        keys.swap(copy.keys);
      }
      return *this;
    }

    StaticTicketKeys& operator=(StaticTicketKeys&& other) noexcept {
      if (this != &other) {
        scrub();
        keys = std::move(other.keys);
      }
      return *this;
    }

    ~StaticTicketKeys() { scrub(); }

    void scrub() noexcept;

    bool operator==(const StaticTicketKeys&) const noexcept = default;

    vector<SessionTicketKey> keys;
  };

 public:
  [[nodiscard]] std::span<const SniCertificate> sniCertificates() const noexcept { return _sniCertificates; }

  [[nodiscard]] std::span<const SessionTicketKey> sessionTicketKeys() const noexcept { return _staticTicketKeys.keys; }

  // Internal storage, read and written through the methods above. Public only so that TLSConfig stays an aggregate,
  // whose members glaze reflects (see glaze-internal-members.hpp).

  // Certificate/key paths and PEM, cipher list, OCSP/CRL inputs, and debug key-log path.
  // Secure: scrubs the private key when it is replaced, copied over or destroyed.
  StaticConcatenatedStrings<kNbStrings, uint32_t, true> _tlsStrings;

  ConcatenatedStrings32 _alpnProtocols;

  // Additional trusted client root / leaf certs (PEM, stored as NUL-separated entries)
  ConcatenatedStrings32 _trustedClientCertsPem;

  vector<SniCertificate> _sniCertificates;
  StaticTicketKeys _staticTicketKeys;
};

}  // namespace aeronet
