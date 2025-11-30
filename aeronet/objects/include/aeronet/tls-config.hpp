#pragma once

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/major-minor-version.hpp"
#include "aeronet/static-concatenated-strings.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

class TLSConfig {
 public:
  using StringViewRange = std::ranges::subrange<SmallConcatenatedStrings::iterator>;

  // RFC 7301 (ALPN) protocol identifier length is encoded in a single octet => maximum 255 bytes.
  // OpenSSL lacks a stable public constant for this; we define it here to avoid magic numbers.
  static constexpr std::size_t kMaxAlpnProtocolLength = 255;
  static constexpr std::size_t kSessionTicketKeySize = 48;

  static constexpr char kTlsVersionPrefix[] = "TLS";

  using Version = MajorMinorVersion<kTlsVersionPrefix>;

  enum class KtlsMode : std::uint8_t { Disabled, Auto, Enabled, Forced };
  enum class CipherPolicy : std::uint8_t { Default, Modern, Compatibility, Legacy };

  using SessionTicketKey = std::array<std::byte, kSessionTicketKeySize>;

  static constexpr Version TLS_1_2 = Version{1, 2};
  static constexpr Version TLS_1_3 = Version{1, 3};

  struct SessionTicketsConfig {
    bool operator==(const SessionTicketsConfig&) const noexcept = default;

    bool enabled{false};
    std::uint32_t maxKeys{2};
    std::chrono::seconds lifetime{std::chrono::hours{24}};
  } sessionTickets;

  struct SniCertificate {
    [[nodiscard]] std::string_view pattern() const noexcept { return _strings[0]; }
    void setPattern(std::string_view value) { _strings.set(0, value); }

    [[nodiscard]] std::string_view certFile() const noexcept { return _strings[1]; }
    [[nodiscard]] auto certFileCstrView() const noexcept { return _strings.makeNullTerminated(1); }
    void setCertFile(std::string_view value) { _strings.set(1, value); }

    [[nodiscard]] std::string_view keyFile() const noexcept { return _strings[2]; }
    [[nodiscard]] auto keyFileCstrView() const noexcept { return _strings.makeNullTerminated(2); }
    void setKeyFile(std::string_view value) { _strings.set(2, value); }

    [[nodiscard]] std::string_view certPem() const noexcept { return _strings[3]; }
    void setCertPem(std::string_view value) { _strings.set(3, value); }

    [[nodiscard]] std::string_view keyPem() const noexcept { return _strings[4]; }
    void setKeyPem(std::string_view value) { _strings.set(4, value); }

    [[nodiscard]] bool hasFiles() const noexcept { return !certFile().empty() || !keyFile().empty(); }
    [[nodiscard]] bool hasPem() const noexcept { return !certPem().empty() || !keyPem().empty(); }

    bool operator==(const SniCertificate&) const noexcept = default;

    bool isWildcard{false};

   private:
    StaticConcatenatedStrings<5, uint32_t> _strings;
  };

  void validate() const;

  // PEM server certificate (may contain chain)
  [[nodiscard]] std::string_view certFile() const noexcept { return _tlsStrings[0]; }
  [[nodiscard]] auto certFileCstrView() const noexcept { return _tlsStrings.makeNullTerminated(0); }

  // PEM private key
  [[nodiscard]] std::string_view keyFile() const noexcept { return _tlsStrings[1]; }
  [[nodiscard]] auto keyFileCstrView() const noexcept { return _tlsStrings.makeNullTerminated(1); }

  // In-memory PEM certificate (used if certFile empty & this non-empty)
  [[nodiscard]] std::string_view certPem() const noexcept { return _tlsStrings[2]; }
  [[nodiscard]] auto certPemCstrView() const noexcept { return _tlsStrings.makeNullTerminated(2); }

  // In-memory PEM private key (used if keyFile empty & this non-empty)
  [[nodiscard]] std::string_view keyPem() const noexcept { return _tlsStrings[3]; }
  [[nodiscard]] auto keyPemCstrView() const noexcept { return _tlsStrings.makeNullTerminated(3); }

  // Optional OpenSSL cipher list string (empty -> default)
  [[nodiscard]] std::string_view cipherList() const noexcept { return _tlsStrings[4]; }
  [[nodiscard]] auto cipherListCstrView() const noexcept { return _tlsStrings.makeNullTerminated(4); }

  TLSConfig& withTlsHandshakeTimeout(std::chrono::milliseconds timeout) {
    handshakeTimeout = timeout;
    return *this;
  }

  TLSConfig& withCertFile(std::string_view certFile) {
    _tlsStrings.set(0, certFile);
    return *this;
  }

  TLSConfig& withKeyFile(std::string_view keyFile) {
    _tlsStrings.set(1, keyFile);
    return *this;
  }

  TLSConfig& withCertPem(std::string_view certPem) {
    _tlsStrings.set(2, certPem);
    return *this;
  }

  TLSConfig& withKeyPem(std::string_view keyPem) {
    _tlsStrings.set(3, keyPem);
    return *this;
  }

  TLSConfig& withCipherList(std::string_view cipherList) {
    _tlsStrings.set(4, cipherList);
    return *this;
  }

  TLSConfig& withTlsCipherPolicy(CipherPolicy policy) {
    cipherPolicy = policy;
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

  TLSConfig& withTlsSessionTicketKey(SessionTicketKey keyMaterial) {
    _staticTicketKeys.push_back(std::move(keyMaterial));
    sessionTickets.enabled = true;
    return *this;
  }

  TLSConfig& clearTlsSessionTicketKeys() {
    _staticTicketKeys.clear();
    return *this;
  }

  TLSConfig& withTlsSniCertificateFiles(std::string_view hostname, std::string_view certPath, std::string_view keyPath);

  TLSConfig& withTlsSniCertificateMemory(std::string_view hostname, std::string_view certPem, std::string_view keyPem);

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

  // Protective timeout for TLS handshakes (time from accept to handshake completion). 0 => disabled.
  std::chrono::milliseconds handshakeTimeout{std::chrono::milliseconds{0}};

  bool enabled{false};            // Master TLS enable/disable switch
  bool requestClientCert{false};  // Request (but not require) a client certificate
  bool requireClientCert{false};  // Require + verify client certificate (strict mTLS). Implies requestClientCert.
  bool alpnMustMatch{false};      // If true and client offers no overlapping ALPN protocol, fail handshake.
  bool logHandshake{false};       // If true, emit log line on TLS handshake completion (ALPN, cipher, version, peer CN)
  bool disableCompression{true};  // Disable TLS-level compression (CRIME mitigation)
  CipherPolicy cipherPolicy{CipherPolicy::Default};

  KtlsMode ktlsMode{
#ifdef AERONET_ENABLE_KTLS
      KtlsMode::Auto
#else
      KtlsMode::Disabled
#endif
  };

  // Optional protocol version bounds (empty => library defaults). Accepted values: "TLS1.2", "TLS1.3".
  Version minVersion;  // If set, enforce minimum TLS protocol version.
  Version maxVersion;  // If set, enforce maximum TLS protocol version.

  std::uint32_t maxConcurrentHandshakes{0};
  std::uint32_t handshakeRateLimitPerSecond{0};
  std::uint32_t handshakeRateLimitBurst{0};

  bool operator==(const TLSConfig&) const noexcept = default;

 private:
  // PEM server certificate, PEM private key, In-memory PEM certificate, In-memory PEM private key, Optional OpenSSL
  // cipher list string
  StaticConcatenatedStrings<5, uint32_t> _tlsStrings;  // Stored TLS-related strings

  SmallConcatenatedStrings _alpnProtocols;

  // Additional trusted client root / leaf certs (PEM, stored as NUL-separated entries)
  SmallConcatenatedStrings _trustedClientCertsPem;

  vector<SniCertificate> _sniCertificates;
  vector<SessionTicketKey> _staticTicketKeys;

 public:
  [[nodiscard]] std::span<const SniCertificate> sniCertificates() const noexcept { return _sniCertificates; }

  [[nodiscard]] std::span<const SessionTicketKey> sessionTicketKeys() const noexcept { return _staticTicketKeys; }
};

}  // namespace aeronet