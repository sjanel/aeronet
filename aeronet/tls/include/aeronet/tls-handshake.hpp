// TLS handshake helpers (factored out of HttpServer implementation)
// Provides a free function to extract negotiated parameters and peer certificate subject
// without depending on HttpServer private internals.
#pragma once

#include <openssl/types.h>

#include <chrono>

#include "aeronet/tls-info.hpp"
#include "aeronet/tls-metrics.hpp"

namespace aeronet {

// Full helper: performs collection, optional logging, populates connection state strings and updates metrics.
// selectedAlpn / negotiatedCipher / negotiatedVersion are mutated in-place.
TLSInfo FinalizeTlsHandshake(const SSL* ssl, int fd, bool logHandshake,
                             std::chrono::steady_clock::time_point handshakeStart, TlsMetricsInternal& metrics);

}  // namespace aeronet
