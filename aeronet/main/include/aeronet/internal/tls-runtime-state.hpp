#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include "aeronet/tls-context.hpp"
#include "aeronet/tls-metrics.hpp"

namespace aeronet::internal {

struct TlsRuntimeState {
  // TlsContext lifetime & pointer stability:
  // ----------------------------------------
  // OpenSSL stores user pointers for callbacks (ALPN selection and SNI routing). These pointers must remain
  // valid for the lifetime of the SSL_CTX and any SSL handshakes using it.
  //
  // For hot reload we keep contexts alive via shared_ptr and each ConnectionState holds a keep-alive to the
  // context it was created from.
  std::shared_ptr<TlsContext> ctxHolder;

  // Optional shared session ticket key store (MultiHttpServer shares one store across instances).
  std::shared_ptr<TlsTicketKeyStore> sharedTicketKeyStore;

  // TLS handshake admission control (basic concurrency + rate limiting).
  uint32_t handshakesInFlight{0};
  uint32_t rateLimitTokens{0};
  std::chrono::steady_clock::time_point rateLimitLastRefill;

  TlsMetricsInternal metrics;  // defined in aeronet/tls-metrics.hpp
};

}  // namespace aeronet::internal