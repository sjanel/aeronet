#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-context.hpp"
#include "aeronet/tls-metrics.hpp"
#include "aeronet/tls-ticket-key-store.hpp"
#include "aeronet/transport-result.hpp"

namespace aeronet::internal {

struct ServerContext;

// The TLS side of the connections of a SingleHttpServer: its TLS context (rebuilt on configuration updates and when
// its files change on disk), the admission control of the handshakes (concurrency and rate limits), the TLS transport
// of the accepted connections, and the report of the handshake outcomes (metrics and handshake callback).
class TlsAcceptor {
 public:
  explicit TlsAcceptor(ServerContext& ctx) noexcept : _pCtx(&ctx) {}

  // Points to the context of the server owning this component, after a move of that context.
  void bind(ServerContext& ctx) noexcept { _pCtx = &ctx; }

  // Builds the TLS context of the configuration, if TLS is enabled.
  void initContext();

  // Whether the server accepts TLS connections.
  [[nodiscard]] bool hasContext() const noexcept { return _ctxHolder != nullptr; }

  // Builds a TLS context from the current configuration, replacing the current context which was built from
  // currentContextConfig (its session ticket keys are kept while the session ticket settings do not change).
  [[nodiscard]] std::shared_ptr<TlsContext> makeContext(const TLSConfig& currentContextConfig) const;

  // Replaces the TLS context used by the next connections (the existing ones keep theirs, see
  // ConnectionState::tlsContextKeepAlive).
  void setContext(std::shared_ptr<TlsContext> ctx) noexcept { _ctxHolder = std::move(ctx); }

  // Rebuilds the TLS context if one of the files it was built from changed on disk, keeping it if the rebuild fails.
  void reloadChangedFiles();

  // Session ticket key store shared by the servers of a MultiHttpServer (null otherwise).
  [[nodiscard]] const std::shared_ptr<TlsTicketKeyStore>& sharedTicketKeyStore() const noexcept {
    return _sharedTicketKeyStore;
  }

  void shareTicketKeyStore(std::shared_ptr<TlsTicketKeyStore> store) noexcept {
    _sharedTicketKeyStore = std::move(store);
  }

  // Admits the TLS handshake of an accepted connection (concurrency and rate limits), then sets up its TLS transport.
  // Returns false if the connection must be closed: handshake rejected or SSL setup failure, both reported.
  bool startHandshake(ConnectionState& state, NativeHandle fd);

  // Records the outcome of the TLS handshake that completed on the connection (negotiated parameters, metrics,
  // handshake event), and releases its admission slot.
  void finalizeHandshake(NativeHandle fd, ConnectionState& state);

  // Reports the failure of the TLS handshake of a connection whose read ended before it completed: EOF (want None) or
  // error. Does nothing for an established connection, or once the outcome was reported.
  void reportInterruptedHandshake(ConnectionState& state, NativeHandle fd, TransportHint want = TransportHint::None);

  // Reports the failure of the TLS handshake of the connection, once.
  void failHandshakeOnce(ConnectionState& state, NativeHandle fd, std::string_view reason) noexcept;

  // Whether the TLS handshake of the connection is still in progress after the handshake timeout.
  [[nodiscard]] bool handshakeTimedOut(const ConnectionState& state,
                                       std::chrono::steady_clock::time_point now) const noexcept;

  // Number of TLS handshakes in progress, counted against the concurrency limit.
  [[nodiscard]] uint32_t nbHandshakesInFlight() const noexcept { return _handshakesInFlight; }

  // The counter of the TLS handshakes in progress, decremented for a connection closed during its handshake.
  [[nodiscard]] uint32_t& handshakesInFlight() noexcept { return _handshakesInFlight; }

  [[nodiscard]] TlsMetricsInternal& metrics() noexcept { return _metrics; }
  [[nodiscard]] const TlsMetricsInternal& metrics() const noexcept { return _metrics; }

 private:
  // Admission control of the handshakes: concurrency and token bucket rate limiting, before allocating OpenSSL objects.
  // Returns false (reported) if the handshake is rejected.
  bool admitHandshake(ConnectionState& state, NativeHandle fd);

  // Session ticket key store for a TLS context built from the current configuration, replacing the current context
  // which was built from currentContextConfig. Null to let the new context create its own.
  [[nodiscard]] std::shared_ptr<TlsTicketKeyStore> ticketKeyStoreForNewContext(
      const TLSConfig& currentContextConfig) const;

  ServerContext* _pCtx;

  // TlsContext lifetime & pointer stability:
  // ----------------------------------------
  // OpenSSL stores user pointers for callbacks (ALPN selection and SNI routing). These pointers must remain
  // valid for the lifetime of the SSL_CTX and any SSL handshakes using it.
  //
  // For hot reload we keep contexts alive via shared_ptr and each ConnectionState holds a keep-alive to the
  // context it was created from.
  std::shared_ptr<TlsContext> _ctxHolder;

  // Optional shared session ticket key store (MultiHttpServer shares one store across instances).
  std::shared_ptr<TlsTicketKeyStore> _sharedTicketKeyStore;

  std::chrono::steady_clock::time_point _rateLimitLastRefill;

  // TLS handshake admission control (basic concurrency + rate limiting).
  uint32_t _handshakesInFlight{0};
  uint32_t _rateLimitTokens{0};

  TlsMetricsInternal _metrics;
};

}  // namespace aeronet::internal
