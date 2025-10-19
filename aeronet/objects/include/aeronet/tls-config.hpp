#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace aeronet {

struct TLSConfig {
  // RFC 7301 (ALPN) protocol identifier length is encoded in a single octet => maximum 255 bytes.
  // OpenSSL lacks a stable public constant for this; we define it here to avoid magic numbers.
  static constexpr std::size_t kMaxAlpnProtocolLength = 255;

  void validate() const;

  bool enabled{false};            // Master TLS enable/disable switch
  bool requestClientCert{false};  // Request (but not require) a client certificate
  bool requireClientCert{false};  // Require + verify client certificate (strict mTLS). Implies requestClientCert.
  bool alpnMustMatch{false};      // If true and client offers no overlapping ALPN protocol, fail handshake.
  bool logHandshake{false};       // If true, emit log line on TLS handshake completion (ALPN, cipher, version, peer CN)

  std::string certFile;    // PEM server certificate (may contain chain)
  std::string keyFile;     // PEM private key
  std::string certPem;     // In-memory PEM certificate (used if certFile empty & this non-empty)
  std::string keyPem;      // In-memory PEM private key (used if keyFile empty & this non-empty)
  std::string cipherList;  // Optional OpenSSL cipher list string (empty -> default)
  // Optional protocol version bounds (empty => library defaults). Accepted values: "TLS1.2", "TLS1.3".
  std::string minVersion;                  // If set, enforce minimum TLS protocol version.
  std::string maxVersion;                  // If set, enforce maximum TLS protocol version.
  std::vector<std::string> alpnProtocols;  // Ordered ALPN protocol list (first match preferred). Empty = disabled.
  std::vector<std::string> trustedClientCertsPem;  // Additional trusted client root / leaf certs (PEM, no files yet)
};

}  // namespace aeronet