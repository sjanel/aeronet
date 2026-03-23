#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/tls-info.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/zerocopy-mode.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-protocol-handler.hpp"
#include "aeronet/protocol-handler.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/types.h>

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

namespace {

#ifdef AERONET_ENABLE_OPENSSL
inline void IncrementTlsFailureReason(TlsMetricsInternal& metrics, std::string_view reason) {
  auto [it, inserted] = metrics.handshakeFailureReasons.emplace(reason, 1);
  if (!inserted) {
    ++(it->second);
  }
}

inline void FailTlsHandshakeOnce(ConnectionState& state, TlsMetricsInternal& metrics, const TlsHandshakeCallback& cb,
                                 NativeHandle fd, std::string_view reason, bool resumed = false,
                                 bool clientCertPresent = false) noexcept {
  if (state.tlsHandshakeEventEmitted) {
    return;
  }
  ++metrics.handshakesFailed;
  IncrementTlsFailureReason(metrics, reason);
  EmitTlsHandshakeEvent(state.tlsInfo, cb, TlsHandshakeEvent::Result::Failed, fd, reason, resumed, clientCertPresent);
  state.tlsHandshakeEventEmitted = true;
}

inline void CheckHandshake(bool isTlsEnabled, ConnectionState& state, TlsMetricsInternal& metrics,
                           const TlsHandshakeCallback& cb, NativeHandle fd, TransportHint want = TransportHint::None) {
  if (isTlsEnabled && !state.tlsEstablished && !state.tlsHandshakeEventEmitted &&
      dynamic_cast<TlsTransport*>(state.transport.get()) != nullptr) {
    std::string_view reason;
    if (state.tlsHandshakeObserver.alpnStrictMismatch) {
      reason = kTlsHandshakeFailureReasonAlpnStrictMismatch;
    } else {
      reason = (want == TransportHint::None) ? kTlsHandshakeFailureReasonEof : kTlsHandshakeFailureReasonError;
    }
    FailTlsHandshakeOnce(state, metrics, cb, fd, reason);
  }
}

#endif

}  // namespace

void SingleHttpServer::sweepIdleConnections() {
  // Periodic maintenance of live connections: applies keep-alive timeout (if enabled) and
  // header read timeout (always, regardless of keep-alive enablement). The header read timeout
  // needs a periodic check because a client might send a partial request line then stall; no
  // further EPOLLIN events will arrive to trigger enforcement in handleReadableClient().
  const auto now = _connections.now;

#ifdef AERONET_WINDOWS
  auto cnxIt = _connections.begin();
  while (cnxIt != _connections.end()) {
#else
  for (auto cnxIt = _connections.begin(); cnxIt != _connections.end(); ++cnxIt) {
    if (!*cnxIt) {
      continue;
    }
#endif
    const NativeHandle fd = cnxIt->fd();
    ConnectionState& state = _connections.connectionState(cnxIt);

    // Retry pending file sends to handle potential missed EPOLLOUT edges.
    if (state.isSendingFile() && state.waitingWritable) {
      flushFilePayload(cnxIt);
    }
    // Retry pending outbound buffer flushes to handle potential missed EPOLLOUT edges.
    // On Windows, WSAPoll can fail to report writability on loopback sockets, leaving
    // buffered response data (including HTTP/2 DATA frames) stuck in outBuffer indefinitely.
    // Periodic retry here ensures forward progress regardless of missed poll events.
    if (!state.outBuffer.empty() && state.waitingWritable) {
      flushOutbound(cnxIt);
    }

    // For DrainThenClose mode, only close after buffers and file payload are fully drained
    if (state.canCloseConnectionForDrain()) {
      closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_drain");
#ifdef AERONET_WINDOWS
      cnxIt = _connections.begin();
#endif
      continue;
    }

    // Keep-alive inactivity enforcement only if enabled.
    // Don't close if there's an active file send - those can block waiting for socket to be writable.
    // Don't close tunnel connections - they can legitimately be idle waiting for data from the
    // remote peer and rely on TCP-level keepalive / peer close detection instead.
    if (_config.enableKeepAlive && !state.isSendingFile() && !state.isTunneling() &&
        now > state.lastActivity + _config.keepAliveTimeout) {
      log::debug("sweepIdleConnections: fd # {} closed for keep-alive timeout", fd);
      closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_keep_alive");
#ifdef AERONET_WINDOWS
      cnxIt = _connections.begin();
#endif
      continue;
    }

    // Header read timeout: active if headerStart set and duration exceeded and no full request parsed yet.
    if (_config.headerReadTimeout.count() > 0 && state.headerStartTp.time_since_epoch().count() != 0 &&
        now > state.headerStartTp + _config.headerReadTimeout) {
      log::debug("sweepIdleConnections: fd # {} closed for header read timeout", fd);
      emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, {});
      closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_header_read_timeout");
#ifdef AERONET_WINDOWS
      cnxIt = _connections.begin();
#endif
      continue;
    }

    // Body read timeout: triggered when the handler is waiting for missing body bytes.
    if (_config.bodyReadTimeout.count() > 0 && state.waitingForBody &&
        state.bodyLastActivity.time_since_epoch().count() != 0 &&
        now > state.bodyLastActivity + _config.bodyReadTimeout) {
      log::debug("sweepIdleConnections: fd # {} closed for body read timeout", fd);
      emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, {});
      closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_body_read_timeout");
#ifdef AERONET_WINDOWS
      cnxIt = _connections.begin();
#endif
      continue;
    }

#ifdef AERONET_ENABLE_OPENSSL
    // TLS handshake timeout (if enabled). Applies only while handshake pending.
    if (_config.tls.handshakeTimeout.count() > 0 && _config.tls.enabled &&
        state.tlsInfo.handshakeStart.time_since_epoch().count() != 0 && !state.tlsEstablished &&
        !state.transport->handshakeDone()) {
      if (now > state.tlsInfo.handshakeStart + _config.tls.handshakeTimeout) {
        FailTlsHandshakeOnce(state, _tls.metrics, _callbacks.tlsHandshake, fd,
                             kTlsHandshakeFailureReasonHandshakeTimeout);
        closeConnection(cnxIt);
        _telemetry.counterAdd("aeronet.connections.closed_for_handshake_timeout");
#ifdef AERONET_WINDOWS
        cnxIt = _connections.begin();
#endif
        continue;
      }
    }
#endif

    state.reclaimMemoryFromOversizedBuffers();

#ifdef AERONET_WINDOWS
    ++cnxIt;
#endif
  }

  _telemetry.gauge("aeronet.connections.cached_count", static_cast<int64_t>(_connections.nbCachedConnections()));

  // Clean up cached connections that have been idle for too long
  _connections.sweepCachedConnections(std::chrono::hours{1});

  _connections.shrink_to_fit();
}

void SingleHttpServer::acceptNewConnections() {
  while (true) {
    Connection cnx(_listenSocket);
    if (!cnx) {
      // no more waiting connections
      break;
    }
    const auto cnxFd = cnx.fd();
    if (_config.tcpNoDelay == TcpNoDelayMode::Enabled) {
      if (!SetTcpNoDelay(cnxFd)) [[unlikely]] {
        const auto err = LastSystemError();
        log::error("setsockopt(TCP_NODELAY) failed for fd # {} err={}", cnxFd, err);
        _telemetry.counterAdd("aeronet.connections.errors.tcp_nodelay_failed", 1UL);
      }
    }
    if (!_eventLoop.add(EventLoop::EventFd{cnxFd, EventIn | EventRdHup | EventEt})) [[unlikely]] {
      _telemetry.counterAdd("aeronet.connections.errors.add_event_failed", 1UL);
      continue;
    }

    auto cnxIt = _connections.emplace(std::move(cnx));

    _telemetry.counterAdd("aeronet.connections.accepted", 1UL);

    ConnectionState& state = _connections.connectionState(cnxIt);

    state.initializeStateNewConnection(_config, cnxFd, _compressionState);

    ZerocopyMode zerocopyMode = _config.zerocopyMode;
    if (!state.zerocopyRequested) {
      zerocopyMode = ZerocopyMode::Disabled;
    }

#ifdef AERONET_ENABLE_OPENSSL
    if (_tls.ctxHolder) {
      // TLS handshake admission control (Phase 2): concurrency and basic token bucket rate limiting.
      // Rejections happen before allocating OpenSSL objects.
      if (_config.tls.maxConcurrentHandshakes != 0 && _tls.handshakesInFlight >= _config.tls.maxConcurrentHandshakes)
          [[unlikely]] {
        ++_tls.metrics.handshakesRejectedConcurrency;
        IncrementTlsFailureReason(_tls.metrics, kTlsHandshakeFailureReasonRejectedConcurrency);
        EmitTlsHandshakeEvent(state.tlsInfo, _callbacks.tlsHandshake, TlsHandshakeEvent::Result::Rejected, cnxFd,
                              kTlsHandshakeFailureReasonRejectedConcurrency);
        closeConnection(cnxIt);
        continue;
      }
      if (_config.tls.handshakeRateLimitPerSecond != 0) {
        const auto burst = (_config.tls.handshakeRateLimitBurst != 0) ? _config.tls.handshakeRateLimitBurst
                                                                      : _config.tls.handshakeRateLimitPerSecond;
        const auto now = state.lastActivity;
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
          EmitTlsHandshakeEvent(state.tlsInfo, _callbacks.tlsHandshake, TlsHandshakeEvent::Result::Rejected, cnxFd,
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

      // OpenSSL's SSL_set_fd takes int; on Windows SOCKET is UINT_PTR but the value
      // round-trips safely through int for sockets allocated by the OS.
      if (AeronetSslSetFd(sslPtr.get(), static_cast<int>(cnxFd)) != 1) [[unlikely]] {  // associate
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
      state.transport = std::make_unique<TlsTransport>(std::move(sslPtr), _config.zerocopyMinBytes);
      state.tlsInfo.handshakeStart = state.lastActivity;
      ++_tls.handshakesInFlight;
    } else {
      state.transport = std::make_unique<PlainTransport>(cnxFd, zerocopyMode, _config.zerocopyMinBytes);
    }
#else
    state.transport = std::make_unique<PlainTransport>(cnxFd, zerocopyMode, _config.zerocopyMinBytes);
#endif
    ConnectionState* pCnx = &state;
    std::size_t bytesReadThisEvent = 0;
    while (true) {
      std::size_t chunkSize = _config.minReadChunkBytes;
      if (_config.maxPerEventReadBytes != 0) {
        if (_config.maxPerEventReadBytes <= bytesReadThisEvent) {
          break;  // fairness cap reached for this epoll cycle
        }
        chunkSize = std::min(chunkSize, _config.maxPerEventReadBytes - bytesReadThisEvent);
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
        // Use per-connection preference determined at accept time
        pCnx->finalizeAndEmitTlsHandshakeIfNeeded(cnxFd, _callbacks.tlsHandshake, _tls.metrics, _config.tls);
        if (pCnx->tlsHandshakeInFlight) {
          pCnx->tlsHandshakeInFlight = false;
          --_tls.handshakesInFlight;
        }
#ifdef AERONET_ENABLE_HTTP2
        // Check for HTTP/2 via ALPN negotiation ("h2")
        if (_config.http2.enable && pCnx->tlsInfo.selectedAlpn() == http2::kAlpnH2) {
          setupHttp2Connection(cnxFd, _config.tcpNoDelay, *pCnx);
        }
#endif
        if (pCnx->isAnyCloseRequested()) {
          closeConnection(cnxIt);
          pCnx = nullptr;
          break;
        }
#endif
        pCnx->tlsEstablished = true;
      }
      if (pCnx->waitingForBody && bytesRead > 0) {
        pCnx->bodyLastActivity = state.lastActivity;
      }
      // Close only on fatal transport error or an orderly EOF (bytesRead==0 with no 'want' hint).
      if (want == TransportHint::Error || (bytesRead == 0 && want == TransportHint::None)) {
        if (want == TransportHint::Error) [[unlikely]] {
          log::error("Closing connection fd # {} bytesRead={} want={} err={}", cnxFd, bytesRead, static_cast<int>(want),
                     LastSystemError());
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
        CheckHandshake(_config.tls.enabled && _tls.ctxHolder, *pCnx, _tls.metrics, _callbacks.tlsHandshake, cnxFd,
                       want);
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
        // For TLS transports: OpenSSL may hold already-decrypted data in its
        // internal buffer that the kernel socket no longer signals via epoll
        // (critical with EPOLLET).  Continue reading if the transport reports
        // pending data — otherwise the server would stall until the peer sends
        // more, causing severe throughput degradation with few connections.
        if (!pCnx->transport->hasPendingReadData()) {
          break;
        }
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

void SingleHttpServer::closeConnection(ConnectionIt cnxIt) {
  const auto cfd = cnxIt->fd();
  ConnectionState& state = _connections.connectionState(cnxIt);
  log::debug("closeConnection called for fd # {}", cfd);

  // If this is a tunnel endpoint (CONNECT), ensure we tear down the peer too.
  // Otherwise, peerFd may dangle and later accidentally match a reused fd, causing
  // spurious epoll_ctl failures and incorrect forwarding.
  const auto peerFd = state.peerFd;
  if (peerFd != kInvalidHandle) {
    auto peerIt = _connections.iterator(peerFd);
#ifdef AERONET_WINDOWS
    if (peerIt != _connections.end()) [[likely]] {
#else
    if (*peerIt) [[likely]] {
#endif
      ConnectionState& peerConnectionState = _connections.connectionState(peerIt);
#ifdef AERONET_ENABLE_HTTP2
      if (state.peerStreamId != 0) {
        // HTTP/2 tunnel upstream being closed: notify the peer's handler to send END_STREAM,
        // but do NOT tear down the peer HTTP/2 connection (it may have other active streams).
        if (peerConnectionState.protocolHandler) {
          auto* h2Handler = static_cast<http2::Http2ProtocolHandler*>(peerConnectionState.protocolHandler.get());
          h2Handler->closeTunnelByUpstreamFd(cfd);
          flushOutbound(peerIt);
        }
      } else
#endif
          if (peerConnectionState.peerFd == cfd) [[likely]] {
        _eventLoop.del(peerFd);
#ifdef AERONET_ENABLE_OPENSSL
        _connections.recycleOrRelease(peerIt, _config.maxCachedConnections, _config.tls.enabled,
                                      _tls.handshakesInFlight);
#else
        _connections.recycleOrRelease(peerIt, _config.maxCachedConnections);
#endif
#ifdef AERONET_WINDOWS
        // bytell_hash_map::erase() can relocate chain-tail elements into the
        // erased slot, invalidating any iterator that pointed at the moved entry.
        // Re-lookup our own iterator so we don't use a stale one below.
        cnxIt = _connections.iterator(cfd);
#endif
      } else {
        log::error("Tunnel peer mismatch while closing fd # {} (peerFd={}, peer.peerFd={})", cfd, peerFd,
                   peerConnectionState.peerFd);
      }
    }
  }

#ifdef AERONET_ENABLE_HTTP2
  // If this connection carries an HTTP/2 handler with active tunnel upstreams, collect their fds
  // before releasing the connection, then close each one (without recursive peer teardown).
  http2::Http2ProtocolHandler::TunnelUpstreamsMap tunnelUpstreamFds;
  if (state.protocolHandler && state.protocolHandler->type() == ProtocolType::Http2) {
    auto* h2Handler = static_cast<http2::Http2ProtocolHandler*>(state.protocolHandler.get());
    tunnelUpstreamFds = h2Handler->drainTunnelUpstreamFds();
  }
#endif

  _eventLoop.del(cfd);
#ifdef AERONET_ENABLE_OPENSSL
  _connections.recycleOrRelease(cnxIt, _config.maxCachedConnections, _config.tls.enabled, _tls.handshakesInFlight);
#else
  _connections.recycleOrRelease(cnxIt, _config.maxCachedConnections);
#endif

#ifdef AERONET_ENABLE_HTTP2
  // Close tunnel upstream fds after the HTTP/2 connection has been released.
  // Set peerFd = -1 on each to prevent them from trying to close the already-released peer.
  for (const auto& [upFd, streamId] : tunnelUpstreamFds) {
    auto upIt = _connections.iterator(upFd);
#ifdef AERONET_WINDOWS
    if (upIt != _connections.end()) {
#else
    if (*upIt) {
#endif
      ConnectionState& upState = _connections.connectionState(upIt);
      upState.peerFd = kInvalidHandle;
      upState.peerStreamId = 0;
      _eventLoop.del(upFd);
#ifdef AERONET_ENABLE_OPENSSL
      _connections.recycleOrRelease(upIt, _config.maxCachedConnections, _config.tls.enabled, _tls.handshakesInFlight);
#else
      _connections.recycleOrRelease(upIt, _config.maxCachedConnections);
#endif
    }
  }
#endif
}

SingleHttpServer::CloseStatus SingleHttpServer::handleWritableClient(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);

  // If this connection was created for an upstream non-blocking connect, and connect is pending,
  // check SO_ERROR to determine whether connect completed successfully or failed.
  const auto fd = cnxIt->fd();
  if (state.connectPending) {
    const int err = GetSocketError(fd);
    state.connectPending = false;
    if (err != 0) {
      // Upstream connect failed. Attempt to notify the client side (peerFd) and close this upstream.
      const auto peerIt = _connections.iterator(state.peerFd);
#ifdef AERONET_WINDOWS
      if (peerIt != _connections.end()) {
#else
      if (*peerIt) {
#endif
#ifdef AERONET_ENABLE_HTTP2
        if (state.peerStreamId != 0) {
          // HTTP/2 tunnel upstream: RST_STREAM the tunnel stream
          ConnectionState& peerState = _connections.connectionState(peerIt);
          auto* h2Handler = static_cast<http2::Http2ProtocolHandler*>(peerState.protocolHandler.get());
          h2Handler->tunnelConnectFailed(state.peerStreamId);
          flushOutbound(peerIt);
        } else
#endif
        {
          emitSimpleError(peerIt, http::StatusCodeBadGateway, "Upstream connect failed");
        }
      } else {
        log::error("Unable to notify client of upstream connect failure: peer fd # {} not found", state.peerFd);
      }
      return CloseStatus::Close;
    }
    // otherwise connect succeeded; continue to normal writable handling
  }
  // If tunneling, flush tunnelOutBuffer first
  if (state.isTunneling() && !state.tunnelOrFileBuffer.empty() && !state.tunnelTransportWrite(fd)) {
    // Fatal error writing tunnel data: close this connection
    return CloseStatus::Close;
  }
  flushOutbound(cnxIt);
  return state.canCloseConnectionForDrain() ? CloseStatus::Close : CloseStatus::Keep;
}

SingleHttpServer::CloseStatus SingleHttpServer::handleReadableClient(ConnectionIt cnxIt) {
  ConnectionState* pCnx = _connections.pConnectionState(cnxIt);
  assert(pCnx != nullptr);

  // NOTE: cnx.outBuffer can legitimately be non-empty when we get EPOLLIN.
  // This happens with partial writes and very commonly with TLS (SSL_read/handshake progress
  // can generate outbound records that must be written before further progress).
  // Opportunistically flush here; if still blocked on write, yield and wait for EPOLLOUT.
  if (!pCnx->outBuffer.empty()) {
    flushOutbound(cnxIt);
    if (!pCnx->outBuffer.empty()) {
      if (!pCnx->waitingWritable) {
        enableWritableInterest(cnxIt);
      }
      return CloseStatus::Keep;
    }
  }

  // If in tunneling mode, read raw bytes and forward to peer
  if (pCnx->isTunneling()) {
#ifdef AERONET_ENABLE_HTTP2
    if (pCnx->peerStreamId != 0) {
      return handleInH2Tunneling(cnxIt);
    }
#endif
    return handleInTunneling(cnxIt);
  }

  std::size_t bytesReadThisEvent = 0;
  const auto fd = cnxIt->fd();
  while (true) {
    std::size_t chunkSize = _config.minReadChunkBytes;
    if (_config.maxPerEventReadBytes != 0) {
      if (_config.maxPerEventReadBytes <= bytesReadThisEvent) {
        break;  // fairness budget exhausted
      }
      chunkSize = std::min(chunkSize, _config.maxPerEventReadBytes - bytesReadThisEvent);
    }

    // Re-set the pointer on each loop iteration in case of connection state reallocations.
    cnxIt = _connections.iterator(fd);
    pCnx = _connections.pConnectionState(fd);

    const auto [count, want] = pCnx->transportRead(chunkSize);
    if (!pCnx->tlsEstablished && pCnx->transport->handshakeDone()) {
#ifdef AERONET_ENABLE_OPENSSL
      pCnx->finalizeAndEmitTlsHandshakeIfNeeded(fd, _callbacks.tlsHandshake, _tls.metrics, _config.tls);
#endif
      pCnx->tlsEstablished = true;
#ifdef AERONET_ENABLE_HTTP2
      // Check for HTTP/2 via ALPN negotiation ("h2")
      if (_config.http2.enable && pCnx->tlsInfo.selectedAlpn() == http2::kAlpnH2) {
        setupHttp2Connection(fd, _config.tcpNoDelay, *pCnx);
      }
#endif

      if (pCnx->isAnyCloseRequested()) {
        return CloseStatus::Close;
      }
    }
    if (pCnx->waitingForBody && count > 0) {
      pCnx->bodyLastActivity = _connections.now;
    }
    if (want == TransportHint::Error) [[unlikely]] {
#ifdef AERONET_ENABLE_OPENSSL
      CheckHandshake(_config.tls.enabled && _tls.ctxHolder, *pCnx, _tls.metrics, _callbacks.tlsHandshake, fd);
#endif
      return CloseStatus::Close;
    }
    if (want != TransportHint::None) {
      // Non-fatal: transport needs the socket to be readable or writable before proceeding.
      if (want == TransportHint::WriteReady && !pCnx->waitingWritable) {
        pCnx->waitingWritable = _eventLoop.mod(EventLoop::EventFd{fd, EventIn | EventOut | EventRdHup | EventEt});
      }
      break;
    }
    if (count == 0) {
#ifdef AERONET_ENABLE_OPENSSL
      CheckHandshake(_config.tls.enabled && _tls.ctxHolder, *pCnx, _tls.metrics, _callbacks.tlsHandshake, fd);
#endif
      return CloseStatus::Close;
    }
    bytesReadThisEvent += static_cast<std::size_t>(count);
    if (_config.maxPerEventReadBytes != 0 && bytesReadThisEvent >= _config.maxPerEventReadBytes) {
      // Reached per-event fairness cap; parse what we have then yield.
      if (processConnectionInput(cnxIt)) {
        break;
      }
      break;
    }
    if (pCnx->inBuffer.size() > _config.maxHeaderBytes + _config.maxBodyBytes) {
      // Distinguish header-only overflow (431) from payload/body overflow (413).
      // If we have not yet parsed the header (no DoubleCRLF found) or the buffer
      // already exceeds the configured header limit, treat this as header-field overflow.
      http::StatusCode code;
      if (pCnx->inBuffer.size() > _config.maxHeaderBytes ||
          std::ranges::search(pCnx->inBuffer, http::DoubleCRLF).empty()) {
        code = http::StatusCodeRequestHeaderFieldsTooLarge;
      } else {
        code = http::StatusCodePayloadTooLarge;
      }
      emitSimpleError(cnxIt, code, {});
      return CloseStatus::Close;
    }
    if (processConnectionInput(cnxIt)) {
      break;
    }
    // Header read timeout enforcement: if headers of current pending request are not complete yet
    // (heuristic: no full request parsed and buffer not empty) and duration exceeded -> close.
    if (_config.headerReadTimeout.count() > 0 && pCnx->headerStartTp.time_since_epoch().count() != 0) {
      if (_connections.now - pCnx->headerStartTp > _config.headerReadTimeout) {
        emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, {});
        return CloseStatus::Close;
      }
    }
  }
  // Try to flush again after reading new data, in case TLS needed the read to proceed with write
  if (!pCnx->outBuffer.empty()) {
    flushOutbound(cnxIt);
  }
  return pCnx->canCloseConnectionForDrain() ? CloseStatus::Close : CloseStatus::Keep;
}

// ============================================================================
// Shared CONNECT tunnel helpers (HTTP/1.1 + HTTP/2)
// ============================================================================

NativeHandle SingleHttpServer::setupTunnelConnection(NativeHandle clientFd, std::string_view host,
                                                     std::string_view port) {
  ConnectResult cres = ConnectTCP(std::span<char>(const_cast<char*>(host.data()), host.size()),
                                  std::span<char>(const_cast<char*>(port.data()), port.size()));
  if (cres.failure) {
    return kInvalidHandle;
  }

  const auto upstreamFd = cres.cnx.fd();

  // Register upstream in event loop for edge-triggered reads and writes so we can detect
  // completion of non-blocking connect (EPOLLOUT) as well as incoming data.
  if (!_eventLoop.add(EventLoop::EventFd{upstreamFd, EventIn | EventOut | EventRdHup | EventEt})) [[unlikely]] {
    return kInvalidHandle;
  }

  // Insert upstream connection state. Inserting may rehash the map — callers must
  // not hold iterators across this call. Duplicate fd for a newly connected socket
  // indicates a library bug (the kernel assigns unique fds for each socket()).
  auto upIt = _connections.emplace(std::move(cres.cnx));
  ConnectionState& state = _connections.connectionState(upIt);

  // Set upstream transport to plain (no TLS). Zerocopy is unconditionally disabled for tunnel
  // transports because buffer lifetimes are not stable — data is read into a reusable inBuffer
  // and forwarded immediately; the kernel may still have pages pinned for DMA when the buffer is
  // reused for the next read, causing data corruption.
  state.transport = std::make_unique<PlainTransport>(upstreamFd, ZerocopyMode::Disabled, 0);
  state.peerFd = clientFd;
  state.connectPending = cres.connectPending;

  return upstreamFd;
}

bool SingleHttpServer::forwardTunnelData(ConnectionIt targetIt, std::string_view data) {
  ConnectionState& target = _connections.connectionState(targetIt);

  // If the target is still connecting, waiting for EPOLLOUT, or has buffered data, just buffer.
  if (target.connectPending || target.waitingWritable || !target.tunnelOrFileBuffer.empty()) {
    target.tunnelOrFileBuffer.append(data);
    if (!target.waitingWritable) {
      enableWritableInterest(targetIt);
    }
    return true;
  }

  // Attempt direct write.
  const auto [written, want] = target.transportWrite(data);
  if (want == TransportHint::Error) [[unlikely]] {
    return false;
  }

  // Buffer any unwritten remainder.
  if (static_cast<std::size_t>(written) < data.size()) {
    target.tunnelOrFileBuffer.append(data.data() + written, data.size() - written);
    if (!target.waitingWritable) {
      enableWritableInterest(targetIt);
    }
  }
  return true;
}

bool SingleHttpServer::forwardTunnelData(ConnectionIt targetIt, RawChars& sourceBuffer) {
  ConnectionState& target = _connections.connectionState(targetIt);

  // If the target is still connecting, waiting for EPOLLOUT, or has buffered data, just buffer.
  // Use swap when the target buffer is empty to avoid a memcpy.
  if (target.connectPending || target.waitingWritable || !target.tunnelOrFileBuffer.empty()) {
    if (target.tunnelOrFileBuffer.empty()) {
      sourceBuffer.swap(target.tunnelOrFileBuffer);
    } else {
      target.tunnelOrFileBuffer.append(sourceBuffer);
      sourceBuffer.clear();
    }
    if (!target.waitingWritable) {
      enableWritableInterest(targetIt);
    }
    return true;
  }

  // Attempt direct write.
  const auto [written, want] = target.transportWrite(std::string_view(sourceBuffer));
  if (want == TransportHint::Error) [[unlikely]] {
    return false;
  }

  // Buffer any unwritten remainder via swap when possible.
  sourceBuffer.erase_front(written);
  if (!sourceBuffer.empty()) {
    if (target.tunnelOrFileBuffer.empty()) {
      sourceBuffer.swap(target.tunnelOrFileBuffer);
    } else {
      target.tunnelOrFileBuffer.append(sourceBuffer);
      sourceBuffer.clear();
    }
    if (!target.waitingWritable) {
      enableWritableInterest(targetIt);
    }
  }
  return true;
}

bool SingleHttpServer::shutdownTunnelPeerWrite(ConnectionIt peerIt) {
  ConnectionState& peer = _connections.connectionState(peerIt);
  peer.shutdownWritePending = true;
  if (peer.tunnelOrFileBuffer.empty()) {
    if (!ShutdownWrite(peerIt->fd())) {
      log::warn("Failed to shutdown write for peer fd # {}", peerIt->fd());
      closeConnection(peerIt);
      return true;  // peerIt and its peer are now recycled — do not touch state
    }
    peer.shutdownWritePending = false;
  }
  return false;
}

// ============================================================================

SingleHttpServer::CloseStatus SingleHttpServer::readTunnelData(ConnectionIt cnxIt, std::size_t& bytesReadThisEvent,
                                                               bool& hitEagain) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  while (!state.eofReceived && state.inBuffer.size() < _config.maxOutboundBufferBytes) {
    const std::size_t chunkSize = _config.minReadChunkBytes;
    const auto [bytesRead, want] = state.transportRead(chunkSize);
    if (want == TransportHint::Error) {
      return CloseStatus::Close;
    }
    if (bytesRead == 0 && want == TransportHint::None) {
      state.eofReceived = true;
      break;
    }
    if (want != TransportHint::None) {
      hitEagain = true;
      break;
    }
    bytesReadThisEvent += bytesRead;
    if (bytesRead < chunkSize) {
      hitEagain = true;
      break;
    }
    if (_config.maxPerEventReadBytes != 0 && bytesReadThisEvent >= _config.maxPerEventReadBytes) {
      // Yield event loop to prevent starvation.
      // We must re-arm EPOLLIN manually since we are edge-triggered and didn't hit EAGAIN.
      state.waitingWritable =
          _eventLoop.mod(EventLoop::EventFd{cnxIt->fd(), EventIn | EventOut | EventRdHup | EventEt});
      hitEagain = true;  // Treat as EAGAIN to yield the event loop
      break;
    }
  }
  return CloseStatus::Keep;
}

SingleHttpServer::CloseStatus SingleHttpServer::handleInTunneling(ConnectionIt cnxIt) {
  const auto selfFd = cnxIt->fd();
  ConnectionState& state = _connections.connectionState(cnxIt);
  std::size_t bytesReadThisEvent = 0;
  bool hitEagain = false;
  if (readTunnelData(cnxIt, bytesReadThisEvent, hitEagain) == CloseStatus::Close) {
    return CloseStatus::Close;
  }

  if (state.inBuffer.empty()) {
    if (state.eofReceived) {
      auto peerIt = _connections.iterator(state.peerFd);
#ifdef AERONET_WINDOWS
      if (peerIt != _connections.end()) {
#else
      if (*peerIt) {
#endif
        if (shutdownTunnelPeerWrite(peerIt)) {
          return CloseStatus::Keep;  // peer closed and cleaned up
        }
      }
      // We don't close the connection immediately, we wait for the peer to close it
      // or for the keep-alive timeout to trigger.
      // But we can stop reading from this side.
      (void)_eventLoop.mod(EventLoop::EventFd{selfFd, EventOut | EventRdHup | EventEt});
    }
    return CloseStatus::Keep;
  }

  auto peerIt = _connections.iterator(state.peerFd);
#ifdef AERONET_WINDOWS
  if (peerIt == _connections.end()) [[unlikely]] {
#else
  if (!*peerIt) [[unlikely]] {
#endif
    return CloseStatus::Close;
  }

  if (!forwardTunnelData(peerIt, state.inBuffer)) [[unlikely]] {
    // Fatal transport error while forwarding to peer: close both sides.
    return CloseStatus::Close;
  }

  if (state.eofReceived) {
    if (shutdownTunnelPeerWrite(peerIt)) {
      return CloseStatus::Keep;  // already cleaned up
    }
    (void)_eventLoop.mod(EventLoop::EventFd{selfFd, EventOut | EventRdHup | EventEt});
  }
  return CloseStatus::Keep;
}

}  // namespace aeronet
