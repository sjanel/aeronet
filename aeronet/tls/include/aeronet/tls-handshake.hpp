// TLS handshake helpers (factored out of SingleHttpServer implementation)
// Provides a free function to extract negotiated parameters and peer certificate subject
// without depending on SingleHttpServer private internals.
#pragma once

#include <openssl/types.h>

#include <chrono>
#include <cstdint>

#include "aeronet/tls-config.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-info.hpp"
#include "aeronet/tls-ktls.hpp"
#include "aeronet/tls-metrics.hpp"

namespace aeronet {

// Full helper: performs collection, optional logging, populates connection state strings and updates metrics.
// selectedAlpn / negotiatedCipher / negotiatedVersion are mutated in-place.
TLSInfo FinalizeTlsHandshake(const SSL* ssl, int fd, bool logHandshake, bool tlsHandshakeEventEmitted,
                             const TlsHandshakeCallback& cb, std::chrono::steady_clock::time_point handshakeStart,
                             TlsMetricsInternal& metrics);

// Emit TLS handshake event.
void EmitTlsHandshakeEvent(const TLSInfo& tlsInfo, const TlsHandshakeCallback& cb, TlsHandshakeEvent::Result result,
                           int fd, std::string_view reason = {}, bool resumed = false, bool clientCertPresent = false);

enum class KtlsApplication : std::uint8_t { Enabled, Disabled, CloseConnection };

// Decide whether to enable kTLS send based on the result of the attempt, the configured mode, and update metrics.
KtlsApplication MaybeEnableKtlsSend(KtlsEnableResult ktlsResult, int fd, TLSConfig::KtlsMode ktlsMode,
                                    TlsMetricsInternal& metrics);

}  // namespace aeronet
