// TLS handshake helpers (factored out of HttpServer implementation)
// Provides a free function to extract negotiated parameters and peer certificate subject
// without depending on HttpServer private internals.
#pragma once

#include <openssl/types.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "tls-metrics.hpp"

namespace aeronet {

struct TlsHandshakeResult {
  std::string selectedAlpn;       // negotiated ALPN protocol (empty if none)
  std::string negotiatedCipher;   // cipher suite
  std::string negotiatedVersion;  // protocol version string
  std::string peerSubject;        // RFC2253 formatted subject if client cert present
  bool clientCertPresent{false};
  uint64_t durationNs{0};  // handshake duration in nanoseconds (0 if start time unset)
};

// Collect negotiated TLS parameters and (optionally) peer subject. The handshakeStart timestamp
// should be the moment the TLS handshake began (steady clock). If it equals the epoch (count()==0)
// durationNs remains 0.
TlsHandshakeResult collectTlsHandshakeInfo(SSL* ssl, std::chrono::steady_clock::time_point handshakeStart);

// Convenience: collect + optionally log in one call. Logging format aligns with server's prior implementation.
TlsHandshakeResult collectAndLogTlsHandshake(SSL* ssl, int fd, bool logHandshake,
                                             std::chrono::steady_clock::time_point handshakeStart);

// Full helper: performs collection, optional logging, populates connection state strings and updates metrics.
// selectedAlpn / negotiatedCipher / negotiatedVersion are mutated in-place.
void finalizeTlsHandshake(SSL* ssl, int fd, bool logHandshake, std::chrono::steady_clock::time_point handshakeStart,
                          std::string& selectedAlpn, std::string& negotiatedCipher, std::string& negotiatedVersion,
                          TlsMetricsInternal& metrics);

}  // namespace aeronet
