#include "aeronet/internal/tls-acceptor.hpp"

#include <openssl/ssl.h>
#include <openssl/types.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-context.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-handshake-failure-reasons.hpp"
#include "aeronet/tls-handshake-observer.hpp"
#include "aeronet/tls-handshake.hpp"
#include "aeronet/tls-metrics.hpp"
#include "aeronet/tls-openssl-callouts.hpp"
#include "aeronet/tls-raii.hpp"
#include "aeronet/tls-ticket-key-store.hpp"
#include "aeronet/tls-transport.hpp"
#include "aeronet/transport-result.hpp"

namespace aeronet::internal {

void TlsAcceptor::initContext() {
  const TLSConfig& tlsConfig = _pCtx->config.tls;
  if (tlsConfig.enabled) {
    _ctxHolder = std::make_shared<TlsContext>(tlsConfig, _sharedTicketKeyStore);
  }
}

std::shared_ptr<TlsContext> TlsAcceptor::makeContext(const TLSConfig& currentContextConfig) const {
  return std::make_shared<TlsContext>(_pCtx->config.tls, ticketKeyStoreForNewContext(currentContextConfig));
}

std::shared_ptr<TlsTicketKeyStore> TlsAcceptor::ticketKeyStoreForNewContext(
    const TLSConfig& currentContextConfig) const {
  if (_sharedTicketKeyStore) {
    // MultiHttpServer worker: one store shared by all the workers.
    return _sharedTicketKeyStore;
  }
  // Keep the keys of the current context while the session ticket settings do not change, so that the tickets it issued
  // still resume after a certificate rotation or another TLS change.
  const TLSConfig& tlsConfig = _pCtx->config.tls;
  if (_ctxHolder && currentContextConfig.sessionTickets == tlsConfig.sessionTickets &&
      std::ranges::equal(currentContextConfig.sessionTicketKeys(), tlsConfig.sessionTicketKeys())) {
    return _ctxHolder->ticketKeyStore();
  }
  return {};
}

void TlsAcceptor::reloadChangedFiles() {
  // Points into the current context: used before it is replaced.
  const std::string_view changedFile = _ctxHolder->changedInputFile();
  if (changedFile.empty()) {
    return;
  }
  try {
    auto newTlsContext = makeContext(_pCtx->config.tls);
    log::info("TLS context reloaded, '{}' changed on disk", changedFile);
    _ctxHolder = std::move(newTlsContext);
  } catch (const std::exception& ex) {
    // Retried at the next config update (the file still differs from the one the current context was built from).
    log::error("TLS context reload failed after '{}' changed on disk, previous TLS context kept: {}", changedFile,
               ex.what());
  }
}

bool TlsAcceptor::admitHandshake(ConnectionState& state, NativeHandle fd) {
  const TLSConfig& tlsConfig = _pCtx->config.tls;
  const TlsHandshakeCallback& handshakeCallback = _pCtx->callbacks.tlsHandshake;
  if (tlsConfig.maxConcurrentHandshakes != 0 && _handshakesInFlight >= tlsConfig.maxConcurrentHandshakes) [[unlikely]] {
    ++_metrics.handshakesRejectedConcurrency;
    _metrics.incrementMapCount(_metrics.handshakeFailureReasons, kTlsHandshakeFailureReasonRejectedConcurrency);
    EmitTlsHandshakeEvent(state.tlsInfo, handshakeCallback, TlsHandshakeEvent::Result::Rejected, fd,
                          kTlsHandshakeFailureReasonRejectedConcurrency);
    return false;
  }
  if (tlsConfig.handshakeRateLimitPerSecond != 0) {
    const auto burst = (tlsConfig.handshakeRateLimitBurst != 0) ? tlsConfig.handshakeRateLimitBurst
                                                                : tlsConfig.handshakeRateLimitPerSecond;
    const auto now = state.lastActivity;
    if (_rateLimitLastRefill.time_since_epoch().count() == 0) {
      _rateLimitLastRefill = now;
      _rateLimitTokens = burst;
    }
    const auto elapsed = now - _rateLimitLastRefill;
    const auto addIntervals = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    if (addIntervals > 0) {
      // In 64 bits: after a long idle period, the 32-bit product would wrap around (to few or no tokens).
      const uint64_t addTokens = static_cast<uint64_t>(addIntervals) * uint64_t{tlsConfig.handshakeRateLimitPerSecond};
      _rateLimitTokens = static_cast<uint32_t>(std::min<uint64_t>(burst, _rateLimitTokens + addTokens));
      _rateLimitLastRefill += std::chrono::seconds{addIntervals};
    }
    if (_rateLimitTokens == 0) [[unlikely]] {
      ++_metrics.handshakesRejectedRateLimit;
      _metrics.incrementMapCount(_metrics.handshakeFailureReasons, kTlsHandshakeFailureReasonRejectedRateLimit);
      EmitTlsHandshakeEvent(state.tlsInfo, handshakeCallback, TlsHandshakeEvent::Result::Rejected, fd,
                            kTlsHandshakeFailureReasonRejectedRateLimit);
      return false;
    }
    --_rateLimitTokens;
  }
  return true;
}

bool TlsAcceptor::startHandshake(ConnectionState& state, NativeHandle fd) {
  // TLS handshake admission control (Phase 2): concurrency and basic token bucket rate limiting.
  // Rejections happen before allocating OpenSSL objects.
  if (!admitHandshake(state, fd)) {
    return false;
  }

  struct StateResetter {
    StateResetter(ConnectionState& state, uint32_t& handshakesInFlight)
        : _state(state), _handshakesInFlight(handshakesInFlight) {
      _state.tlsHandshakeInFlight = true;
      _state.tlsHandshakeObserver = {};
      _state.tlsHandshakeEventEmitted = false;
      ++_handshakesInFlight;
    }

    StateResetter(const StateResetter&) = delete;
    StateResetter(StateResetter&&) = delete;
    StateResetter& operator=(const StateResetter&) noexcept = delete;
    StateResetter& operator=(StateResetter&&) noexcept = delete;

    ~StateResetter() {
      if (!ok) {
        _state.tlsHandshakeInFlight = false;
        --_handshakesInFlight;
      }
    }

    ConnectionState& _state;
    uint32_t& _handshakesInFlight;
    bool ok{false};
  };

  state.tlsContextKeepAlive = _ctxHolder;

  StateResetter stateResetter(state, _handshakesInFlight);

  SSL_CTX* ctx = reinterpret_cast<SSL_CTX*>(_ctxHolder->raw());
  SslPtr sslPtr(AeronetSslNew(ctx), ::SSL_free);
  if (sslPtr.get() == nullptr) [[unlikely]] {
    log::error("SSL_new failed for fd # {}", fd);
    failHandshakeOnce(state, fd, kTlsHandshakeFailureReasonSslNewFailed);
    return false;
  }

  // Install per-connection observer for OpenSSL callbacks.
  if (SetTlsHandshakeObserver(reinterpret_cast<ssl_st*>(sslPtr.get()), &state.tlsHandshakeObserver) != 1) [[unlikely]] {
    log::error("SSL_set_ex_data failed to install TLS handshake observer for fd # {}", fd);
    // Treat this as a handshake failure: record metrics, emit event, and close the connection.
    failHandshakeOnce(state, fd, kTlsHandshakeFailureReasonSetExDataFailed);
    return false;
  }

  // OpenSSL's SSL_set_fd takes int; on Windows SOCKET is UINT_PTR but the value
  // round-trips safely through int for sockets allocated by the OS.
  if (AeronetSslSetFd(sslPtr.get(), static_cast<int>(fd)) != 1) [[unlikely]] {  // associate
    log::error("SSL_set_fd failed for fd # {}", fd);
    failHandshakeOnce(state, fd, kTlsHandshakeFailureReasonSslSetFdFailed);
    return false;
  }
  if (AeronetSslSetAcceptState(sslPtr.get()) != 1) [[unlikely]] {
    log::error("SSL_set_accept_state failed for fd # {}", fd);
    failHandshakeOnce(state, fd, kTlsHandshakeFailureReasonSslSetAcceptStateFailed);
    return false;
  }
  state.transport = std::make_unique<TlsTransport>(std::move(sslPtr));
  state.tlsInfo.handshakeStart = state.lastActivity;
  stateResetter.ok = true;
  return true;
}

void TlsAcceptor::finalizeHandshake(NativeHandle fd, ConnectionState& state) {
  state.finalizeAndEmitTlsHandshakeIfNeeded(fd, _pCtx->callbacks.tlsHandshake, _metrics, _pCtx->config.tls);
  if (state.tlsHandshakeInFlight) {
    state.tlsHandshakeInFlight = false;
    --_handshakesInFlight;
  }
}

void TlsAcceptor::reportInterruptedHandshake(ConnectionState& state, NativeHandle fd, TransportHint want) {
  if (_pCtx->config.tls.enabled && _ctxHolder && !state.tlsEstablished && !state.tlsHandshakeEventEmitted &&
      state.transport.isTls()) {
    std::string_view reason;
    if (state.tlsHandshakeObserver.alpnStrictMismatch) {
      ++_metrics.alpnStrictMismatches;
      reason = kTlsHandshakeFailureReasonAlpnStrictMismatch;
    } else {
      reason = (want == TransportHint::None) ? kTlsHandshakeFailureReasonEof : kTlsHandshakeFailureReasonError;
    }
    failHandshakeOnce(state, fd, reason);
  }
}

void TlsAcceptor::failHandshakeOnce(ConnectionState& state, NativeHandle fd, std::string_view reason) noexcept {
  if (state.tlsHandshakeEventEmitted) {
    return;
  }
  ++_metrics.handshakesFailed;
  _metrics.incrementMapCount(_metrics.handshakeFailureReasons, reason);
  EmitTlsHandshakeEvent(state.tlsInfo, _pCtx->callbacks.tlsHandshake, TlsHandshakeEvent::Result::Failed, fd, reason);
  state.tlsHandshakeEventEmitted = true;
}

bool TlsAcceptor::handshakeTimedOut(const ConnectionState& state,
                                    std::chrono::steady_clock::time_point now) const noexcept {
  const TLSConfig& tlsConfig = _pCtx->config.tls;
  return tlsConfig.handshakeTimeout.count() > 0 && tlsConfig.enabled &&
         state.tlsInfo.handshakeStart.time_since_epoch().count() != 0 && !state.tlsEstablished &&
         !state.transport.handshakeDone() && now > state.tlsInfo.handshakeStart + tlsConfig.handshakeTimeout;
}

}  // namespace aeronet::internal
