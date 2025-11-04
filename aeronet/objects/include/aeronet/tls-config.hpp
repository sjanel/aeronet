#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "major-minor-version.hpp"
#include "static-concatenated-strings.hpp"

namespace aeronet {

class TLSConfig {
 public:
  // RFC 7301 (ALPN) protocol identifier length is encoded in a single octet => maximum 255 bytes.
  // OpenSSL lacks a stable public constant for this; we define it here to avoid magic numbers.
  static constexpr std::size_t kMaxAlpnProtocolLength = 255;

  static constexpr char kTlsVersionPrefix[] = "TLS";

  using Version = MajorMinorVersion<kTlsVersionPrefix>;

  static constexpr Version TLS_1_2 = Version{1, 2};
  static constexpr Version TLS_1_3 = Version{1, 3};

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

  bool enabled{false};            // Master TLS enable/disable switch
  bool requestClientCert{false};  // Request (but not require) a client certificate
  bool requireClientCert{false};  // Require + verify client certificate (strict mTLS). Implies requestClientCert.
  bool alpnMustMatch{false};      // If true and client offers no overlapping ALPN protocol, fail handshake.
  bool logHandshake{false};       // If true, emit log line on TLS handshake completion (ALPN, cipher, version, peer CN)

  // Optional protocol version bounds (empty => library defaults). Accepted values: "TLS1.2", "TLS1.3".
  Version minVersion;                      // If set, enforce minimum TLS protocol version.
  Version maxVersion;                      // If set, enforce maximum TLS protocol version.
  std::vector<std::string> alpnProtocols;  // Ordered ALPN protocol list (first match preferred). Empty = disabled.
  std::vector<std::string> trustedClientCertsPem;  // Additional trusted client root / leaf certs (PEM, no files yet)

 private:
  // PEM server certificate, PEM private key, In-memory PEM certificate, In-memory PEM private key, Optional OpenSSL
  // cipher list string
  StaticConcatenatedStrings<5, uint32_t> _tlsStrings;  // Stored TLS-related strings
};

}  // namespace aeronet