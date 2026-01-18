#include <asm-generic/socket.h>
#include <netinet/in.h>  //NOLINT(misc-include-cleaner) used by socket options
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-status-code.hpp"
#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-protocol-handler.hpp"
#endif
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/tls-info.hpp"
#include "aeronet/transport.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/types.h>

#include "aeronet/http-server-config.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-context.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-handshake-failure-reasons.hpp"
#include "aeronet/tls-handshake-observer.hpp"
#include "aeronet/tls-handshake.hpp"
#include "aeronet/tls-metrics.hpp"
#include "aeronet/tls-openssl-callouts.hpp"
#include "aeronet/tls-raii.hpp"
#include "aeronet/tls-transport.hpp"  // from tls module include directory
#endif

namespace aeronet {

#ifdef AERONET_ENABLE_OPENSSL
namespace {
inline void IncrementTlsFailureReason(TlsMetricsInternal& metrics, std::string_view reason) {
  auto [it, inserted] = metrics.handshakeFailureReasons.emplace(reason, 1);
  if (!inserted) {
    ++(it->second);
  }
}

inline void FailTlsHandshakeOnce(ConnectionState& state, TlsMetricsInternal& metrics, const TlsHandshakeCallback& cb,
                                 int fd, std::string_view reason, bool resumed = false,
                                 bool clientCertPresent = false) noexcept {
  if (state.tlsHandshakeEventEmitted) {
    return;
  }
  ++metrics.handshakesFailed;
  IncrementTlsFailureReason(metrics, reason);
  EmitTlsHandshakeEvent(state.tlsHandshakeEventEmitted, state.tlsInfo, cb, TlsHandshakeEvent::Result::Failed, fd,
                        reason, resumed, clientCertPresent);
}

}  // namespace
#endif

void SingleHttpServer::sweepIdleConnections() {
  // Periodic maintenance of live connections: applies keep-alive timeout (if enabled) and
  // header read timeout (always, regardless of keep-alive enablement). The header read timeout
  // needs a periodic check because a client might send a partial request line then stall; no
  // further EPOLLIN events will arrive to trigger enforcement in handleReadableClient().
  const auto now = std::chrono::steady_clock::now();
  for (auto cnxIt = _connections.active.begin(); cnxIt != _connections.active.end();) {
    ConnectionState& state = *cnxIt->second;

    // Retry pending file sends to handle potential missed EPOLLOUT edges.
    if (state.isSendingFile() && state.waitingWritable) {
      flushFilePayload(cnxIt);
    }

    // Close immediately if requested
    if (state.isImmediateCloseRequested()) {
      cnxIt = closeConnection(cnxIt);
      continue;
    }

    // For DrainThenClose mode, only close after buffers and file payload are fully drained
    if (state.canCloseConnectionForDrain()) {
      cnxIt = closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_drain");
      continue;
    }

    // Keep-alive inactivity enforcement only if enabled.
    // Don't close if there's an active file send - those can block waiting for socket to be writable.
    if (_config.enableKeepAlive && !state.isSendingFile() && now > state.lastActivity + _config.keepAliveTimeout) {
      cnxIt = closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_keep_alive");
      continue;
    }

    // Header read timeout: active if headerStart set and duration exceeded and no full request parsed yet.
    if (_config.headerReadTimeout.count() > 0 && state.headerStartTp.time_since_epoch().count() != 0 &&
        now > state.headerStartTp + _config.headerReadTimeout) {
      emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, true, {});
      cnxIt = closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_header_read_timeout");
      continue;
    }

    // Body read timeout: triggered when the handler is waiting for missing body bytes.
    if (_config.bodyReadTimeout.count() > 0 && state.waitingForBody &&
        state.bodyLastActivity.time_since_epoch().count() != 0 &&
        now > state.bodyLastActivity + _config.bodyReadTimeout) {
      emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, true, {});
      cnxIt = closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_body_read_timeout");
      continue;
    }

#ifdef AERONET_ENABLE_OPENSSL
    // TLS handshake timeout (if enabled). Applies only while handshake pending.
    if (_config.tls.handshakeTimeout.count() > 0 && _config.tls.enabled &&
        state.tlsInfo.handshakeStart.time_since_epoch().count() != 0 && !state.tlsEstablished &&
        !state.transport->handshakeDone()) {
      if (now > state.tlsInfo.handshakeStart + _config.tls.handshakeTimeout) {
        FailTlsHandshakeOnce(state, _tls.metrics, _callbacks.tlsHandshake, cnxIt->first.fd(),
                             kTlsHandshakeFailureReasonHandshakeTimeout);
        cnxIt = closeConnection(cnxIt);
        _telemetry.counterAdd("aeronet.connections.closed_for_handshake_timeout");
        continue;
      }
    }
#endif

    ++cnxIt;
  }

  _telemetry.gauge("aeronet.connections.cached_count", static_cast<int64_t>(_connections.nbCachedConnections()));

  // Clean up cached connections that have been idle for too long
  _connections.sweepCachedConnections(now, std::chrono::hours{1});
}

void SingleHttpServer::acceptNewConnections() {
  while (true) {
    Connection cnx(_listenSocket);
    if (!cnx) {
      // no more waiting connections
      break;
    }
    int cnxFd = cnx.fd();
    if (_config.tcpNoDelay) {
      static constexpr int kEnable = 1;
      if (::setsockopt(cnxFd, IPPROTO_TCP, TCP_NODELAY, &kEnable, sizeof(kEnable)) != 0) [[unlikely]] {
        const auto err = errno;
        log::error("setsockopt(TCP_NODELAY) failed for fd # {} err={} ({})", cnxFd, err, std::strerror(err));
        _telemetry.counterAdd("aeronet.connections.errors.tcp_nodelay_failed", 1UL);
      }
    }
    if (!_eventLoop.add(EventLoop::EventFd{cnxFd, EventIn | EventRdHup | EventEt})) [[unlikely]] {
      _telemetry.counterAdd("aeronet.connections.errors.add_event_failed", 1UL);
      continue;
    }

    auto [cnxIt, inserted] = _connections.emplace(std::move(cnx));
    // Note: Duplicate fd on accept indicates a library bug - the kernel assigns unique fds for each
    // accept(), and we remove closed connections from the map before their fd can be reused.
    // Using assert to document this invariant rather than silently handling an impossible case.
    assert(inserted && "Duplicate fd on accept indicates library bug - connection not properly removed");

    _telemetry.counterAdd("aeronet.connections.accepted", 1UL);

    ConnectionState& state = *cnxIt->second;
    state.request._pGlobalHeaders = &_config.globalHeaders;
    state.request._addTrailerHeader = _config.addTrailerHeader;
#ifdef AERONET_ENABLE_OPENSSL
    if (_tls.ctxHolder) {
      // TLS handshake admission control (Phase 2): concurrency and basic token bucket rate limiting.
      // Rejections happen before allocating OpenSSL objects.
      if (_config.tls.maxConcurrentHandshakes != 0 && _tls.handshakesInFlight >= _config.tls.maxConcurrentHandshakes)
          [[unlikely]] {
        ++_tls.metrics.handshakesRejectedConcurrency;
        IncrementTlsFailureReason(_tls.metrics, kTlsHandshakeFailureReasonRejectedConcurrency);
        EmitTlsHandshakeEvent(state.tlsHandshakeEventEmitted, state.tlsInfo, _callbacks.tlsHandshake,
                              TlsHandshakeEvent::Result::Rejected, cnxFd,
                              kTlsHandshakeFailureReasonRejectedConcurrency);
        closeConnection(cnxIt);
        continue;
      }
      if (_config.tls.handshakeRateLimitPerSecond != 0) {
        const auto burst = (_config.tls.handshakeRateLimitBurst != 0) ? _config.tls.handshakeRateLimitBurst
                                                                      : _config.tls.handshakeRateLimitPerSecond;
        const auto now = std::chrono::steady_clock::now();
        if (_tls.rateLimitLastRefill.time_since_epoch().count() == 0) {
          _tls.rateLimitLastRefill = now;
          _tls.rateLimitTokens = burst;
        }
        const auto elapsed = now - _tls.rateLimitLastRefill;
        const auto addIntervals = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        if (addIntervals > 0) {
          const uint32_t addTokens = static_cast<uint32_t>(addIntervals) * _config.tls.handshakeRateLimitPerSecond;
          _tls.rateLimitTokens = std::min(burst, _tls.rateLimitTokens + addTokens);
          _tls.rateLimitLastRefill += std::chrono::seconds{addIntervals};
        }
        if (_tls.rateLimitTokens == 0) [[unlikely]] {
          ++_tls.metrics.handshakesRejectedRateLimit;
          IncrementTlsFailureReason(_tls.metrics, kTlsHandshakeFailureReasonRejectedRateLimit);
          EmitTlsHandshakeEvent(state.tlsHandshakeEventEmitted, state.tlsInfo, _callbacks.tlsHandshake,
                                TlsHandshakeEvent::Result::Rejected, cnxFd,
                                kTlsHandshakeFailureReasonRejectedRateLimit);
          closeConnection(cnxIt);
          continue;
        }
        --_tls.rateLimitTokens;
      }

      state.tlsContextKeepAlive = _tls.ctxHolder;
      state.tlsHandshakeInFlight = true;
      state.tlsHandshakeObserver = {};
      state.tlsHandshakeEventEmitted = false;

      SSL_CTX* ctx = reinterpret_cast<SSL_CTX*>(_tls.ctxHolder->raw());
      SslPtr sslPtr(AeronetSslNew(ctx), ::SSL_free);
      if (sslPtr.get() == nullptr) [[unlikely]] {
        log::error("SSL_new failed for fd # {}", cnxFd);
        FailTlsHandshakeOnce(state, _tls.metrics, _callbacks.tlsHandshake, cnxFd,
                             kTlsHandshakeFailureReasonSslNewFailed);
        closeConnection(cnxIt);
        continue;
      }

      // Install per-connection observer for OpenSSL callbacks.
      if (SetTlsHandshakeObserver(reinterpret_cast<ssl_st*>(sslPtr.get()), &state.tlsHandshakeObserver) != 1)
          [[unlikely]] {
        log::error("SSL_set_ex_data failed to install TLS handshake observer for fd # {}", cnxFd);
        // Treat this as a handshake failure: record metrics, emit event, and close the connection.
        FailTlsHandshakeOnce(state, _tls.metrics, _callbacks.tlsHandshake, cnxFd,
                             kTlsHandshakeFailureReasonSetExDataFailed);
        closeConnection(cnxIt);
        continue;
      }

      if (AeronetSslSetFd(sslPtr.get(), cnxFd) != 1) [[unlikely]] {  // associate
        log::error("SSL_set_fd failed for fd # {}", cnxFd);
        FailTlsHandshakeOnce(state, _tls.metrics, _callbacks.tlsHandshake, cnxFd,
                             kTlsHandshakeFailureReasonSslSetFdFailed);
        closeConnection(cnxIt);
        continue;
      }
      // Enable partial writes: SSL_write will return after writing some data rather than
      // trying to write everything. This is crucial for non-blocking I/O performance.
      SSL_set_mode(sslPtr.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
      ::SSL_set_accept_state(sslPtr.get());
      state.transport = std::make_unique<TlsTransport>(std::move(sslPtr));
      state.tlsInfo.handshakeStart = std::chrono::steady_clock::now();
      ++_tls.handshakesInFlight;
    } else {
      state.transport = std::make_unique<PlainTransport>(cnxFd);
    }
#else
    state.transport = std::make_unique<PlainTransport>(cnxFd);
#endif
    ConnectionState* pCnx = &state;
    std::size_t bytesReadThisEvent = 0;
    while (true) {
      std::size_t chunkSize = _config.minReadChunkBytes;
      if (_config.maxPerEventReadBytes != 0) {
        std::size_t remainingBudget = (_config.maxPerEventReadBytes > bytesReadThisEvent)
                                          ? (_config.maxPerEventReadBytes - bytesReadThisEvent)
                                          : 0;
        if (remainingBudget == 0) {
          break;  // fairness cap reached for this epoll cycle
        }
        chunkSize = std::min(chunkSize, remainingBudget);
      }
      const auto [bytesRead, want] = pCnx->transportRead(chunkSize);
      // Check for handshake completion
      // If the TLS handshake completed during the preceding transportRead, finalize it
      // immediately so we capture negotiated ALPN/cipher/version/client-cert and update
      // metrics/state. This must be done even if the same read later returns an error or
      // EOF — the handshake result is valuable and should be recorded before any
      // connection teardown logic runs.
      // Note: this is a transition action (handshakePending -> done) rather than a
      // normal successful-read action, so it intentionally runs prior to evaluating
      // transport error/EOF handling below.
      if (!pCnx->tlsEstablished && pCnx->transport->handshakeDone()) {
#ifdef AERONET_ENABLE_OPENSSL
        pCnx->finalizeAndEmitTlsHandshakeIfNeeded(cnxFd, _callbacks.tlsHandshake, _tls.metrics, _config.tls);
        if (pCnx->tlsHandshakeInFlight) {
          pCnx->tlsHandshakeInFlight = false;
          --_tls.handshakesInFlight;
        }
#ifdef AERONET_ENABLE_HTTP2
        // Check for HTTP/2 via ALPN negotiation ("h2")
        if (_config.http2.enable && pCnx->tlsInfo.selectedAlpn() == http2::kAlpnH2) {
          setupHttp2Connection(*pCnx);
        }
#endif
        if (pCnx->isImmediateCloseRequested()) {
          cnxIt = closeConnection(cnxIt);
          pCnx = nullptr;
          break;
        }
#endif
        pCnx->tlsEstablished = true;
      }
      if (pCnx->waitingForBody && bytesRead > 0) {
        pCnx->bodyLastActivity = std::chrono::steady_clock::now();
      }
      // Close only on fatal transport error or an orderly EOF (bytesRead==0 with no 'want' hint).
      if (want == TransportHint::Error || (bytesRead == 0 && want == TransportHint::None)) {
        // If TLS handshake still pending, treat a transport Error as transient and retry later.
        if (want == TransportHint::Error) [[unlikely]] {
#ifdef AERONET_ENABLE_OPENSSL
          if (pCnx->transport && !pCnx->transport->handshakeDone()) {
            log::debug("Transient transport error during TLS handshake on fd # {}; will retry", cnxFd);
            // Yield and let event loop drive readiness notifications; do not close yet.
            break;
          }  // Emit richer diagnostics to aid debugging TLS handshake / transport failures.
#endif
          log::error("Closing connection fd # {} bytesRead={} want={} errno={} ({})", cnxFd, bytesRead,
                     static_cast<int>(want), errno, std::strerror(errno));
#ifdef AERONET_ENABLE_OPENSSL
          if (_tls.ctxHolder) {
            auto* tlsTr = dynamic_cast<TlsTransport*>(pCnx->transport.get());
            if (tlsTr != nullptr) {
              const SSL* ssl = tlsTr->rawSsl();
              if (ssl != nullptr) {
                const char* ver = ::SSL_get_version(ssl);
                const char* cipher = ::SSL_get_cipher_name(ssl);
                log::error("TLS state fd # {} ver={} cipher={}", cnxFd, (ver != nullptr) ? ver : "?",
                           (cipher != nullptr) ? cipher : "?");
              }
              tlsTr->logErrorIfAny();
            }
          }
#endif
        }

#ifdef AERONET_ENABLE_OPENSSL
        if (_config.tls.enabled && !pCnx->tlsEstablished && _tls.ctxHolder &&
            dynamic_cast<TlsTransport*>(pCnx->transport.get()) != nullptr && !pCnx->tlsHandshakeEventEmitted) {
          std::string_view reason;
          if (pCnx->tlsHandshakeObserver.alpnStrictMismatch) {
            reason = kTlsHandshakeFailureReasonAlpnStrictMismatch;
          } else {
            reason = (want == TransportHint::None) ? kTlsHandshakeFailureReasonEof : kTlsHandshakeFailureReasonError;
          }
          FailTlsHandshakeOnce(*pCnx, _tls.metrics, _callbacks.tlsHandshake, cnxFd, reason);
        }
#endif
        closeConnection(cnxIt);
        pCnx = nullptr;
        break;
      }
      if (want != TransportHint::None) {
        // Transport indicates we should wait for readability or writability before continuing.
        // Adjust epoll interest if TLS handshake needs write readiness
        if (want == TransportHint::WriteReady && !pCnx->waitingWritable) {
          pCnx->waitingWritable = _eventLoop.mod(EventLoop::EventFd{cnxFd, EventIn | EventOut | EventRdHup | EventEt});
        }
        break;
      }
      bytesReadThisEvent += static_cast<std::size_t>(bytesRead);
      _telemetry.counterAdd("aeronet.bytes.read", static_cast<uint64_t>(bytesRead));
      if (bytesRead < chunkSize) {
        break;
      }
      if (_config.maxPerEventReadBytes != 0 && bytesReadThisEvent >= _config.maxPerEventReadBytes) {
        break;  // reached fairness cap
      }
    }
    if (pCnx == nullptr) {
      continue;
    }

    const bool closeNow = processConnectionInput(cnxIt);
    if (closeNow && pCnx->outBuffer.empty() && pCnx->tunnelOrFileBuffer.empty() && !pCnx->isSendingFile()) {
      closeConnection(cnxIt);
    }
  }
}

SingleHttpServer::ConnectionMapIt SingleHttpServer::closeConnection(ConnectionMapIt cnxIt) {
  const int cfd = cnxIt->first.fd();

  // If this is a tunnel endpoint (CONNECT), ensure we tear down the peer too.
  // Otherwise, peerFd may dangle and later accidentally match a reused fd, causing
  // spurious epoll_ctl failures and incorrect forwarding.
  const int peerFd = cnxIt->second->peerFd;
  if (peerFd != -1) {
    auto peerIt = _connections.active.find(peerFd);
    if (peerIt != _connections.active.end()) [[likely]] {
      if (peerIt->second->peerFd == cfd) [[likely]] {
        _eventLoop.del(peerFd);
#ifdef AERONET_ENABLE_OPENSSL
        _connections.recycleOrRelease(_config.maxCachedConnections, _config.tls.enabled, peerIt,
                                      _tls.handshakesInFlight);
#else
        _connections.recycleOrRelease(_config.maxCachedConnections, peerIt);
#endif
      } else {
        log::error("Tunnel peer mismatch while closing fd # {} (peerFd={}, peer.peerFd={})", cfd, peerFd,
                   peerIt->second->peerFd);
      }
    }
  }

  _eventLoop.del(cfd);
#ifdef AERONET_ENABLE_OPENSSL
  return _connections.recycleOrRelease(_config.maxCachedConnections, _config.tls.enabled, cnxIt,
                                       _tls.handshakesInFlight);
#else
  return _connections.recycleOrRelease(_config.maxCachedConnections, cnxIt);
#endif
}

void SingleHttpServer::handleWritableClient(int fd) {
  auto cnxIt = _connections.active.find(fd);
  if (cnxIt == _connections.active.end()) [[unlikely]] {
    log::error("Received an invalid fd # {} from the event loop (or already removed?)", fd);
    return;
  }

  ConnectionState& state = *cnxIt->second;
  // If this connection was created for an upstream non-blocking connect, and connect is pending,
  // check SO_ERROR to determine whether connect completed successfully or failed.
  if (state.connectPending) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1) [[unlikely]] {
      log::error("getsockopt(SO_ERROR) failed for fd # {} errno={} ({})", fd, errno, std::strerror(errno));
      // Treat as connect failure
      err = errno;
    }
    state.connectPending = false;
    if (err != 0) {
      // Upstream connect failed. Attempt to notify the client side (peerFd) with 502 and close both.
      const auto peerIt = _connections.active.find(state.peerFd);
      if (peerIt != _connections.active.end()) {
        emitSimpleError(peerIt, http::StatusCodeBadGateway, true, "Upstream connect failed");
      } else {
        log::error("Unable to notify client of upstream connect failure: peer fd # {} not found", state.peerFd);
      }
      closeConnection(cnxIt);
      return;
    }
    // otherwise connect succeeded; continue to normal writable handling
  }
  // If tunneling, flush tunnelOutBuffer first
  if (state.isTunneling() && !state.tunnelOrFileBuffer.empty()) {
    const auto [written, want] = state.transportWrite(state.tunnelOrFileBuffer);
    if (want == TransportHint::Error) [[unlikely]] {
      // Fatal error writing tunnel data: close this connection
      closeConnection(cnxIt);
      return;
    }
    state.tunnelOrFileBuffer.erase_front(written);
    // If still has data, keep EPOLLOUT registered
    if (!state.tunnelOrFileBuffer.empty()) {
      return;
    }
    // Tunnel buffer drained: fall through to normal flushOutbound handling
  }
  flushOutbound(cnxIt);
  if (state.canCloseImmediately()) {
    closeConnection(cnxIt);
  }
}

void SingleHttpServer::handleReadableClient(int fd) {
  auto cnxIt = _connections.active.find(fd);
  if (cnxIt == _connections.active.end()) [[unlikely]] {
    log::error("Received an invalid fd # {} from the event loop (or already removed?)", fd);
    return;
  }

  const auto nowTime = std::chrono::steady_clock::now();
  ConnectionState& cnx = *cnxIt->second;
  cnx.lastActivity = nowTime;

  // NOTE: cnx.outBuffer can legitimately be non-empty when we get EPOLLIN.
  // This happens with partial writes and very commonly with TLS (SSL_read/handshake progress
  // can generate outbound records that must be written before further progress).
  // Opportunistically flush here; if still blocked on write, yield and wait for EPOLLOUT.
  if (!cnx.outBuffer.empty()) {
    flushOutbound(cnxIt);
    if (!cnx.outBuffer.empty()) {
      if (!cnx.waitingWritable) {
        enableWritableInterest(cnxIt);
      }
      return;
    }
  }

  // If in tunneling mode, read raw bytes and forward to peer
  if (cnx.isTunneling()) {
    handleInTunneling(cnxIt);
    return;
  }

  std::size_t bytesReadThisEvent = 0;
  while (true) {
    std::size_t chunkSize = _config.minReadChunkBytes;
    if (_config.maxPerEventReadBytes != 0) {
      std::size_t remainingBudget =
          (_config.maxPerEventReadBytes > bytesReadThisEvent) ? (_config.maxPerEventReadBytes - bytesReadThisEvent) : 0;
      if (remainingBudget == 0) {
        break;  // fairness budget exhausted
      }
      chunkSize = std::min(chunkSize, remainingBudget);
    }
    const auto [count, want] = cnx.transportRead(chunkSize);
    if (!cnx.tlsEstablished && cnx.transport->handshakeDone()) {
#ifdef AERONET_ENABLE_OPENSSL
      cnx.finalizeAndEmitTlsHandshakeIfNeeded(fd, _callbacks.tlsHandshake, _tls.metrics, _config.tls);
#endif
      cnx.tlsEstablished = true;

#ifdef AERONET_ENABLE_HTTP2
      // Check for HTTP/2 via ALPN negotiation ("h2")
      if (_config.http2.enable && cnx.tlsInfo.selectedAlpn() == http2::kAlpnH2) {
        setupHttp2Connection(cnx);
      }
#endif

      if (cnx.isImmediateCloseRequested()) {
        closeConnection(cnxIt);
        return;
      }
    }
    if (cnx.waitingForBody && count > 0) {
      cnx.bodyLastActivity = nowTime;
    }
    if (want == TransportHint::Error) [[unlikely]] {
#ifdef AERONET_ENABLE_OPENSSL
      if (_config.tls.enabled && !cnx.tlsEstablished && _tls.ctxHolder &&
          dynamic_cast<TlsTransport*>(cnx.transport.get()) != nullptr && !cnx.tlsHandshakeEventEmitted) {
        std::string_view reason = cnx.tlsHandshakeObserver.alpnStrictMismatch
                                      ? kTlsHandshakeFailureReasonAlpnStrictMismatch
                                      : kTlsHandshakeFailureReasonError;
        FailTlsHandshakeOnce(cnx, _tls.metrics, _callbacks.tlsHandshake, fd, reason);
      }
#endif
      cnx.requestImmediateClose();
      break;
    }
    if (want != TransportHint::None) {
      // Non-fatal: transport needs the socket to be readable or writable before proceeding.
      if (want == TransportHint::WriteReady && !cnx.waitingWritable) {
        cnx.waitingWritable = _eventLoop.mod(EventLoop::EventFd{fd, EventIn | EventOut | EventRdHup | EventEt});
      }
      break;
    }
    if (count == 0) {
#ifdef AERONET_ENABLE_OPENSSL
      if (_config.tls.enabled && !cnx.tlsEstablished && _tls.ctxHolder &&
          dynamic_cast<TlsTransport*>(cnx.transport.get()) != nullptr && !cnx.tlsHandshakeEventEmitted) {
        FailTlsHandshakeOnce(cnx, _tls.metrics, _callbacks.tlsHandshake, fd, kTlsHandshakeFailureReasonEof);
      }
#endif
      cnx.requestImmediateClose();
      break;
    }
    bytesReadThisEvent += static_cast<std::size_t>(count);
    if (_config.maxPerEventReadBytes != 0 && bytesReadThisEvent >= _config.maxPerEventReadBytes) {
      // Reached per-event fairness cap; parse what we have then yield.
      if (processConnectionInput(cnxIt)) {
        break;
      }
      break;
    }
    if (cnx.inBuffer.size() > _config.maxHeaderBytes + _config.maxBodyBytes) {
      // Distinguish header-only overflow (431) from payload/body overflow (413).
      // If we have not yet parsed the header (no DoubleCRLF found) or the buffer
      // already exceeds the configured header limit, treat this as header-field overflow.
      http::StatusCode code;
      if (std::ranges::search(cnx.inBuffer, http::DoubleCRLF).empty() || cnx.inBuffer.size() > _config.maxHeaderBytes) {
        code = http::StatusCodeRequestHeaderFieldsTooLarge;
      } else {
        code = http::StatusCodePayloadTooLarge;
      }
      emitSimpleError(cnxIt, code, false, {});
      cnx.requestImmediateClose();
      break;
    }
    if (processConnectionInput(cnxIt)) {
      break;
    }
    // Header read timeout enforcement: if headers of current pending request are not complete yet
    // (heuristic: no full request parsed and buffer not empty) and duration exceeded -> close.
    if (_config.headerReadTimeout.count() > 0 && cnx.headerStartTp.time_since_epoch().count() != 0) {
      if (nowTime - cnx.headerStartTp > _config.headerReadTimeout) {
        emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, false, {});
        cnx.requestImmediateClose();
        break;
      }
    }
  }
  // Try to flush again after reading new data, in case TLS needed the read to proceed with write
  if (!cnx.outBuffer.empty()) {
    flushOutbound(cnxIt);
  }
  if (cnx.canCloseImmediately()) {
    closeConnection(cnxIt);
  }
}

void SingleHttpServer::handleInTunneling(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  std::size_t bytesReadThisEvent = 0;
  while (true) {
    const std::size_t chunk = _config.minReadChunkBytes;
    const auto [bytesRead, want] = state.transportRead(chunk);
    if (want == TransportHint::Error || (bytesRead == 0 && want == TransportHint::None)) {
      closeConnection(cnxIt);
      return;
    }
    if (want != TransportHint::None) {
      break;
    }
    bytesReadThisEvent += bytesRead;
    if (bytesRead < chunk) {
      break;
    }
    if (_config.maxPerEventReadBytes != 0 && bytesReadThisEvent >= _config.maxPerEventReadBytes) {
      break;
    }
  }
  assert(!state.inBuffer.empty());
  auto peerIt = _connections.active.find(state.peerFd);
  if (peerIt == _connections.active.end()) [[unlikely]] {
    closeConnection(cnxIt);
    return;
  }
  ConnectionState& peer = *peerIt->second;
  const auto [written, want] = peer.transportWrite(state.inBuffer);
  if (want == TransportHint::Error) [[unlikely]] {
    // Fatal transport error while forwarding to peer: close both sides.
    closeConnection(cnxIt);
    return;
  }
  state.inBuffer.erase_front(written);
  if (!state.inBuffer.empty()) {
    if (peer.tunnelOrFileBuffer.empty()) {
      state.inBuffer.swap(peer.tunnelOrFileBuffer);
    } else {
      peer.tunnelOrFileBuffer.append(state.inBuffer);
      state.inBuffer.clear();
    }

    if (!peer.waitingWritable) {
      enableWritableInterest(peerIt);
    }
  }
}

}  // namespace aeronet
