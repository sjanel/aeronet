#ifdef AERONET_WINDOWS
#include <ws2tcpip.h>
#else
#include <sys/socket.h>  // sockaddr_storage
#endif

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
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/keep-alive-deadline-queue.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/tls-info.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zerocopy-mode.hpp"
#include "tunnel-manager.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-protocol-handler.hpp"
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

#ifdef AERONET_ENABLE_TEST_HOOKS
#include "aeronet/transport-test-hook.hpp"
#endif

namespace aeronet {

namespace {

#ifdef AERONET_ENABLE_OPENSSL
inline void IncrementTlsFailureReason(TlsMetricsInternal& metrics, std::string_view reason) {
  metrics.incrementMapCount(metrics.handshakeFailureReasons, reason);
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
  if (isTlsEnabled && !state.tlsEstablished && !state.tlsHandshakeEventEmitted && state.transport.isTls()) {
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

void SingleHttpServer::refreshKeepAliveDeadline(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
#ifdef AERONET_ENABLE_WEBSOCKET
  // keepAliveTimeout bounds the idleness between HTTP requests: an upgraded connection has its own timeouts.
  if (state.protocol == ProtocolType::WebSocket) {
    refreshWebSocketDeadline(cnxIt->fd(), state);
    return;
  }
#endif
  // A CONNECT waiting for its target resolution is answered once getaddrinfo returns, whatever the time it takes.
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  const auto* asyncState = state.pAsyncState();
  bool asyncHandlerActive = asyncState != nullptr && asyncState->active;
  if (!_config.enableKeepAlive || state.isTunneling() || state.tunnelResolving || asyncHandlerActive) {
#else
  if (!_config.enableKeepAlive || state.isTunneling() || state.tunnelResolving) {
#endif
    _keepAliveDeadlines.remove(state);
    return;
  }
  // An armed deadline is never later than lastActivity + keepAliveTimeout: lastActivity only moves forward, and a
  // configuration update rebuilds the queue. Keep it: closeExpiredKeepAliveConnections() re-arms it from lastActivity
  // when it fires, so that an active connection costs one heap update per keepAliveTimeout instead of one per event.
  if (!internal::KeepAliveDeadlineQueue::contains(state)) {
    _keepAliveDeadlines.upsert(state, cnxIt->fd(), state.lastActivity + _config.keepAliveTimeout);
  }
}

bool SingleHttpServer::isServerInternalFd(NativeHandle fd) const noexcept {
  return fd == _listenSocket.fd() || fd == _lifecycle.wakeupFd.fd() || fd == _maintenanceTimer.fd();
}

void SingleHttpServer::restartKeepAliveIdleWindows(std::span<const EventLoop::EventFd> events,
                                                   std::chrono::steady_clock::time_point batchStart) {
  if (!_config.enableKeepAlive) {
    return;
  }
  const auto workEnd = std::chrono::steady_clock::now();
  if (batchStart + _config.keepAliveTimeout > workEnd) {
    return;  // the whole batch was served well within the idle window: no armed deadline can be stale
  }
  for (const auto event : events) {
    if (!isServerInternalFd(event.fd)) {
      restartKeepAliveIdleWindow(event.fd, workEnd);
    }
  }
}

void SingleHttpServer::restartKeepAliveIdleWindow(NativeHandle fd, std::chrono::steady_clock::time_point workEnd) {
  if (!_config.enableKeepAlive) {
    return;
  }
  const auto cnxIt = _connections.iterator(fd);
  if (!IsValid(_connections, cnxIt)) {
    return;
  }
  ConnectionState& state = _connections.connectionState(cnxIt);
  // The deadline armed when the request was read is stale as soon as serving it took longer than
  // keepAliveTimeout - a multi-second database query in a synchronous handler is enough. keepAliveTimeout
  // bounds idleness *between* requests (see HttpServerConfig::keepAliveTimeout), so restart the idle window
  // from the instant the work completed. Without this the next maintenance sweep would close the connection
  // right after the response was produced, before the peer had any chance to read it.
  if (state.lastActivity + _config.keepAliveTimeout > workEnd) {
    return;  // served well within the idle window: the armed deadline is still valid
  }
  state.lastActivity = workEnd;
  refreshKeepAliveDeadline(cnxIt);
}

bool SingleHttpServer::closeExpiredKeepAliveConnections() {
  bool closedAny = false;
  const auto now = _connections.now;

  while (!_keepAliveDeadlines.empty()) {
    const auto& next = _keepAliveDeadlines.top();
    if (next.expiresAt > now) {
      break;
    }

    const auto expired = _keepAliveDeadlines.pop();
    auto cnxIt = _connections.iterator(expired.fd);
    if (!IsValid(_connections, cnxIt) || _connections.pConnectionState(cnxIt) != expired.pState) [[unlikely]] {
      continue;
    }

    ConnectionState& state = _connections.connectionState(cnxIt);
#ifdef AERONET_ENABLE_WEBSOCKET
    if (state.protocol == ProtocolType::WebSocket) {
      closedAny |= checkWebSocketTimeouts(cnxIt);
      continue;
    }
#endif
    if (!_config.enableKeepAlive || state.isTunneling() || state.tunnelResolving) {
      continue;
    }

#ifdef AERONET_ENABLE_HTTP2
    // keepAliveTimeout applies between requests, after the previous response has completed. An active HTTP/2
    // stream may be waiting for peer flow-control credit and therefore produce no socket events at all; reaping
    // it here would truncate a response at the current window boundary. Recheck periodically until all streams
    // complete, at which point the last event-refreshed deadline resumes normal idle expiry.
    if (state.protocolHandler && state.protocolHandler->type() == ProtocolType::Http2 &&
        static_cast<http2::Http2ProtocolHandler*>(state.protocolHandler.get())->connection().activeStreamCount() != 0) {
      _keepAliveDeadlines.upsert(state, expired.fd, now + _config.keepAliveTimeout);
      continue;
    }
#endif

    // File sends are exempt because their progress is driven by this very sweep (flushFilePayload retries
    // above), which does not refresh lastActivity.
    if (state.isSendingFile()) {
      _keepAliveDeadlines.upsert(state, expired.fd, now + _config.keepAliveTimeout);
      continue;
    }

    const auto currentExpiry = state.lastActivity + _config.keepAliveTimeout;
    if (currentExpiry > now) {
      _keepAliveDeadlines.upsert(state, expired.fd, currentExpiry);
      continue;
    }

    log::debug("sweepIdleConnections: fd # {} closed for keep-alive timeout", expired.fd);
    closeConnection(cnxIt);
    _telemetry.counterAdd("aeronet.connections.closed_for_keep_alive");
    closedAny = true;
  }

  return closedAny;
}

void SingleHttpServer::rebuildKeepAliveDeadlines() {
  _keepAliveDeadlines.clear();
  // Not skipped when keep-alive is disabled: WebSocket connections have their own timeouts.
  for (auto cnxIt = _connections.begin(); cnxIt != _connections.end(); ++cnxIt) {
    if (!IsValid(_connections, cnxIt)) {
      continue;
    }
    refreshKeepAliveDeadline(cnxIt);
  }
}

void SingleHttpServer::clearRequestDeadline(ConnectionState& state) noexcept {
  if (state.requestDeadlineMs == ConnectionState::kInactiveRelativeMs) {
    return;
  }
  state.requestDeadlineMs = ConnectionState::kInactiveRelativeMs;
  assert(_connectionSweepState.requestDeadlineConnections > 0U);
  --_connectionSweepState.requestDeadlineConnections;
}

void SingleHttpServer::trackRequestDeadline(ConnectionState& state, uint32_t deadlineMs) noexcept {
  if (state.requestDeadlineMs == ConnectionState::kInactiveRelativeMs) {
    ++_connectionSweepState.requestDeadlineConnections;
  }
  state.requestDeadlineMs = deadlineMs;
}

void SingleHttpServer::forgetWritableInterest(ConnectionState& state) noexcept {
  if (!state.waitingWritable) {
    return;
  }
  state.waitingWritable = false;
  assert(_connectionSweepState.writableConnections > 0U);
  --_connectionSweepState.writableConnections;
}

void SingleHttpServer::forgetConnectionMaintenance(ConnectionState& state) noexcept {
  _keepAliveDeadlines.remove(state);
  clearRequestDeadline(state);
  forgetWritableInterest(state);
  if (state.parsingHeaders && _config.headerReadTimeout.count() > 0) {
    assert(_connectionSweepState.pendingTimeoutConnections > 0U);
    --_connectionSweepState.pendingTimeoutConnections;
  }
  state.parsingHeaders = false;
  if (state.waitingForBody) {
    assert(_connectionSweepState.pendingTimeoutConnections > 0U);
    --_connectionSweepState.pendingTimeoutConnections;
    state.waitingForBody = false;
  }
#ifdef AERONET_ENABLE_HTTP2
  if (state.protocol == ProtocolType::Http2) {
    assert(_connectionSweepState.http2Connections > 0U);
    --_connectionSweepState.http2Connections;
  }
#endif
}

bool SingleHttpServer::needsFullConnectionMaintenanceSweep() const noexcept {
  if (_connections.empty()) {
    return false;
  }
  if (_lifecycle.isDraining() || _lifecycle.isStopping()) {
    return true;
  }
  if (_connectionSweepState.writableConnections != 0U || _connectionSweepState.requestDeadlineConnections != 0U) {
    return true;
  }
  if (_connectionSweepState.pendingTimeoutConnections != 0U) {
    return true;
  }
#ifdef AERONET_ENABLE_OPENSSL
  if (_config.tls.enabled && _config.tls.handshakeTimeout.count() > 0) {
    return true;
  }
#endif
#ifdef AERONET_ENABLE_HTTP2
  if (_connectionSweepState.http2Connections != 0U) {
    return true;
  }
#endif
  return false;
}

void SingleHttpServer::sweepIdleConnections() {
  // Periodic maintenance of live connections. Keep-alive idle timeout is handled by
  // _keepAliveDeadlines so idle HTTP/1.1 connections do not require an O(n) scan.
  // The full scan is reserved for states that still need periodic observation:
  // missed writable edges, header/body/request/TLS timeouts, HTTP/2 stream deadlines,
  // and graceful-drain closure.
  const auto now = _connections.now;
  closeExpiredKeepAliveConnections();

  if (!needsFullConnectionMaintenanceSweep()) {
    _telemetry.gauge("aeronet.connections.cached_count", static_cast<int64_t>(_connections.nbCachedConnections()));
    _connections.sweepCachedConnections(std::chrono::hours{1});
    return;
  }

  // Cap the number of sendfile / outbound retries per sweep to avoid spending the
  // entire maintenance tick retrying N connections that all return EAGAIN immediately.
  static constexpr int kMaxFlushRetriesPerSweep = 128;
  int flushRetries = 0;

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
    if (state.isSendingFile() && state.waitingWritable && flushRetries < kMaxFlushRetriesPerSweep) {
      flushFilePayload(cnxIt);
      resumeInputIfOutputDrained(fd, state);
      ++flushRetries;
    }
    // Retry pending outbound buffer flushes to handle potential missed EPOLLOUT edges.
    // On Windows, WSAPoll can fail to report writability on loopback sockets, leaving
    // buffered response data (including HTTP/2 DATA frames) stuck in outBuffer indefinitely.
    // Periodic retry here ensures forward progress regardless of missed poll events.
    if (state.hasPendingOutput() && state.waitingWritable && flushRetries < kMaxFlushRetriesPerSweep) {
      flushOutbound(cnxIt);
      ++flushRetries;
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

    // Header read timeout: active while headers are being parsed and duration exceeded.
    if (_config.headerReadTimeout.count() > 0 && state.parsingHeaders &&
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
        state.bodyLastActivityMs != ConnectionState::kInactiveRelativeMs &&
        now > state.headerStartTp + std::chrono::milliseconds(state.bodyLastActivityMs) + _config.bodyReadTimeout) {
      log::debug("sweepIdleConnections: fd # {} closed for body read timeout", fd);
      emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, {});
      closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_body_read_timeout");
#ifdef AERONET_WINDOWS
      cnxIt = _connections.begin();
#endif
      continue;
    }

    // Per-route request timeout: active when a per-route deadline was set after routing.
    if (state.requestDeadlineMs != ConnectionState::kInactiveRelativeMs &&
        now > state.headerStartTp + std::chrono::milliseconds(state.requestDeadlineMs)) {
      log::debug("sweepIdleConnections: fd # {} closed for per-route request timeout", fd);
      emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, {});
      closeConnection(cnxIt);
      _telemetry.counterAdd("aeronet.connections.closed_for_request_timeout");
#ifdef AERONET_WINDOWS
      cnxIt = _connections.begin();
#endif
      continue;
    }

#ifdef AERONET_ENABLE_OPENSSL
    // TLS handshake timeout (if enabled). Applies only while handshake pending.
    if (_config.tls.handshakeTimeout.count() > 0 && _config.tls.enabled &&
        state.tlsInfo.handshakeStart.time_since_epoch().count() != 0 && !state.tlsEstablished &&
        !state.transport.handshakeDone()) {
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

#ifdef AERONET_ENABLE_HTTP2
    // Sweep per-stream request deadlines for HTTP/2 connections.
    if (state.protocol == ProtocolType::Http2 && state.protocolHandler) {
      static_cast<http2::Http2ProtocolHandler*>(state.protocolHandler.get())->sweepStreams(now);
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
  // Cap the number of connections accepted per event-loop iteration to avoid starving
  // existing connections when a burst of new connections arrives (e.g. wrk opening 1000
  // connections simultaneously).  Remaining connections stay in the kernel backlog and
  // will be accepted on the next EPOLLIN on the listen socket.
  const auto maxAcceptBatch = _config.maxAcceptBatchSize;
  assert(maxAcceptBatch > 0);
  for (decltype(_config.maxAcceptBatchSize) accepted = 0; accepted < maxAcceptBatch;) {
    sockaddr_storage peerAddress;
    Connection cnx(_listenSocket, peerAddress);
    if (!cnx) {
      // no more waiting connections
      break;
    }
    const auto cnxFd = cnx.fd();
    bool tcpNoDelayActive = false;
    if (_config.tcpNoDelay == TcpNoDelayMode::Enabled) {
      if (SetTcpNoDelay(cnxFd)) [[likely]] {
        tcpNoDelayActive = true;
      } else {
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

    state.initializeStateNewConnection(_config, peerAddress, _compressionState);

    // TCP_NODELAY disables Nagle — mark corkable so response writes use TCP_CORK to coalesce.
    state.corkable = tcpNoDelayActive;

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
      ::SSL_set_accept_state(sslPtr.get());
      state.transport = std::make_unique<TlsTransport>(std::move(sslPtr), _config.zerocopyMinBytes);
      state.tlsInfo.handshakeStart = state.lastActivity;
      ++_tls.handshakesInFlight;
    } else {
      state.transport = Transport(cnxFd, zerocopyMode, _config.zerocopyMinBytes);
    }
#else
    state.transport = Transport(cnxFd, zerocopyMode, _config.zerocopyMinBytes);
#endif

#ifdef AERONET_ENABLE_TEST_HOOKS
    // Test hook: allow tests to wrap/decorate the transport for fault injection.
    state.transport = test::ApplyTransportDecorator(std::move(state.transport));
#endif

    refreshKeepAliveDeadline(cnxIt);

    ConnectionState* pCnx = &state;
    std::size_t bytesReadThisEvent = 0;
    // Input that the loop below left to the deferred read: the rest of the socket data beyond the fairness budget, or
    // what the transport still reports after a short read.
    bool inputLeft = false;
    while (true) {
      const std::size_t chunkSize = _config.computeReadChunkSize(bytesReadThisEvent);
      assert(chunkSize > 0);
      const bool wasParsing = pCnx->parsingHeaders;
      const auto [bytesRead, want] = pCnx->transportRead(chunkSize);
      if (!wasParsing && pCnx->parsingHeaders && _config.headerReadTimeout.count() > 0) {
        ++_connectionSweepState.pendingTimeoutConnections;
      }
      // Check for handshake completion
      // If the TLS handshake completed during the preceding transportRead, finalize it
      // immediately so we capture negotiated ALPN/cipher/version/client-cert and update
      // metrics/state. This must be done even if the same read later returns an error or
      // EOF - the handshake result is valuable and should be recorded before any
      // connection teardown logic runs.
      // Note: this is a transition action (handshakePending -> done) rather than a
      // normal successful-read action, so it intentionally runs prior to evaluating
      // transport error/EOF handling below.
      if (finalizeTlsHandshakeIfReady(cnxFd, *pCnx)) {
        closeConnection(cnxIt);
        pCnx = nullptr;
        break;
      }
      if (pCnx->waitingForBody && bytesRead > 0) {
        pCnx->bodyLastActivityMs = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(state.lastActivity - pCnx->headerStartTp).count());
      }
      // Close only on fatal transport error or an orderly EOF (bytesRead==0 with no 'want' hint).
      if (want == TransportHint::Error || (bytesRead == 0 && want == TransportHint::None)) {
        if (want == TransportHint::Error) [[unlikely]] {
          log::error("Closing connection fd # {} bytesRead={} want={} err={}", cnxFd, bytesRead, static_cast<int>(want),
                     LastSystemError());
#ifdef AERONET_ENABLE_OPENSSL
          if (_tls.ctxHolder) {
            auto* tlsTr = pCnx->transport.get<TlsTransport>();
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
        if (want == TransportHint::WriteReady && !pCnx->waitingWritable && !enableWritableInterest(cnxIt)) {
          closeConnection(cnxIt);
          pCnx = nullptr;
        }
        break;
      }
      bytesReadThisEvent += static_cast<std::size_t>(bytesRead);
      _telemetry.counterAdd("aeronet.bytes.read", static_cast<uint64_t>(bytesRead));
      if (bytesRead < chunkSize) {
        // The socket is drained, but the transport can still report pending input, that the kernel does not signal
        // again (edge-triggered polling): what the TLS read-ahead buffered behind the returned data, like a peer
        // close_notify sent with its request. It is read once the input read so far is served: reading it now would
        // close the connection on the close_notify before answering the request.
        inputLeft = pCnx->transport.hasPendingReadData();
        break;
      }
      if (_config.fairnessBudgetExhausted(bytesReadThisEvent)) {
        inputLeft = true;
        break;
      }
    }
    if (pCnx == nullptr) {
      continue;
    }

    bool closeNow = processConnectionInput(cnxIt);
    // CONNECT setup can grow the POSIX fd-indexed connection vectors and invalidate cnxIt. It can also leave tunnel
    // bytes that arrived with the request head in inBuffer. Re-find the connection and forward those bytes before
    // returning to edge-triggered polling, where no further read edge would be guaranteed.
    cnxIt = _connections.iterator(cnxFd);
    pCnx = _connections.pConnectionState(cnxFd);
    // The input left by the read loop is resumed after the other ready connections, as in handleReadableClient(): a
    // client that completed its handshake and sent its request before the accept may have had all of it read ahead by
    // the TLS transport, which no read event reports. Unless the input processing stopped reading (it is then resumed
    // with it), or the tunnel forwarding below reads the rest.
    const bool deferRemainingInput = inputLeft && !closeNow && !pCnx->isTunneling();
    if (!closeNow && pCnx->isTunneling()) {
      closeNow = _tunnels->relayInput(cnxIt) == CloseStatus::Close;
    } else if (closeNow && !pCnx->isAnyCloseRequested()) {
      // Input processing only stopped reading (an async handler holding the input, blocked output...).
      closeNow = false;
    }
    if (closeNow && !pCnx->hasPendingOutput() && pCnx->tunnelOrFileBuffer.empty() && !pCnx->isSendingFile()) {
      // With zerocopy sends still in flight, close once they completed (see canCloseConnectionForDrain()) instead of
      // aborting them (see closeConnection()).
      pCnx->requestDrainAndClose();
      closeNow = !pCnx->hasZerocopySendsInFlight();
    } else {
      closeNow = false;
    }
    if (closeNow) {
      closeConnection(cnxIt);
    } else {
      if (deferRemainingInput) {
        deferInput(cnxFd, *pCnx);
      }
      // A client that pipelines its first request with the connection setup (h2c prior knowledge sends
      // preface + SETTINGS + HEADERS immediately) gets it dispatched right here, so the handler can outlive
      // keepAliveTimeout on this path too.
      restartKeepAliveIdleWindow(cnxFd);
    }

    ++accepted;
  }
}

void SingleHttpServer::closeConnection(ConnectionIt cnxIt) {
  const auto cfd = cnxIt->fd();

  log::debug("closeConnection called for fd # {}", cfd);

  forgetConnectionMaintenance(_connections.connectionState(cnxIt));
  if (_tunnels != nullptr) {
    _tunnels->closeTunnelsOf(cnxIt);
    // Releasing the other ends of its tunnels may have moved the entries of the connection storage (Windows).
    cnxIt = _connections.iterator(cfd);
  }
  releaseConnection(cnxIt);
}

void SingleHttpServer::releaseConnection(ConnectionIt cnxIt) {
  const auto fd = cnxIt->fd();
  ConnectionState& state = _connections.connectionState(cnxIt);
  _eventLoop.del(fd);
  // The buffers of zerocopy sends still in flight are released with the connection state, while the kernel would keep
  // sending from them after a graceful close: abort such a connection (reset) so that the kernel drops them first.
  // Drain closes wait for the completions instead (see ConnectionState::canCloseConnectionForDrain()).
  if (state.hasZerocopySendsInFlight()) {
    log::debug("Aborting fd # {} with zerocopy sends still in flight", fd);
    if (!SetAbortiveClose(fd)) [[unlikely]] {
      log::error("setsockopt(SO_LINGER) failed for fd # {} err={}", fd, LastSystemError());
    }
    _telemetry.counterAdd("aeronet.connections.aborted_with_zerocopy_in_flight");
  }
#ifdef AERONET_ENABLE_OPENSSL
  _connections.recycleOrRelease(cnxIt, _config.maxCachedConnections, _config.tls.enabled, _tls.handshakesInFlight);
#else
  _connections.recycleOrRelease(cnxIt, _config.maxCachedConnections);
#endif
}

bool SingleHttpServer::finalizeTlsHandshakeIfReady([[maybe_unused]] NativeHandle fd, ConnectionState& state) {
  if (state.tlsEstablished || !state.transport.handshakeDone()) {
    return false;
  }
#ifdef AERONET_ENABLE_OPENSSL
  state.finalizeAndEmitTlsHandshakeIfNeeded(fd, _callbacks.tlsHandshake, _tls.metrics, _config.tls);
  if (state.tlsHandshakeInFlight) {
    state.tlsHandshakeInFlight = false;
    --_tls.handshakesInFlight;
  }
#ifdef AERONET_ENABLE_HTTP2
  if (_config.http2.enable && state.tlsInfo.selectedAlpn() == http2::kAlpnH2) {
    setupHttp2Connection(fd, _config.tcpNoDelay, state);
  }
#endif
#endif
  state.tlsEstablished = true;
  return state.isAnyCloseRequested();
}

SingleHttpServer::CloseStatus SingleHttpServer::handleWritableClient(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);

  const auto fd = cnxIt->fd();
  if (state.isTunneling() && _tunnels->handleWritable(cnxIt) == CloseStatus::Close) {
    return CloseStatus::Close;
  }
  flushOutbound(cnxIt);

  // Once EPOLLOUT drained the output, resume the input left by its backpressure directly: edge-triggered polling may
  // not deliver another read event.
  processBufferedInput(fd);
  ConnectionState* pState = _connections.pConnectionState(fd);
  assert(pState != nullptr);  // the input processing only requests closes
  // The records that the TLS transport read ahead while the input processing was stopped are not reported by the
  // kernel either.
  if (!pState->hasPendingOutput() && !pState->isAnyCloseRequested() && pState->transport.hasPendingReadData()) {
    deferInput(fd, *pState);
  }
  return pState->canCloseConnectionForDrain() ? CloseStatus::Close : CloseStatus::Keep;
}

void SingleHttpServer::processBufferedInput(NativeHandle fd) {
  // Output backpressure can leave complete input in the user-space input buffer: HTTP/2 frames above the output
  // high-water mark, or HTTP/1 requests pipelined behind a response that waited for the socket (not processed after a
  // close request). So can the fairness budget, once the responses to the pipelined HTTP/1 requests reached it.
  ConnectionState* pState = _connections.pConnectionState(fd);
  if (pState == nullptr || pState->hasPendingOutput() || pState->inBuffer.empty()) {
    return;
  }
  if (pState->protocolHandler) {
    (void)processSpecialProtocolHandler(_connections.iterator(fd));
  } else if (!pState->isSendingFile() && !pState->isTunneling() && !pState->isAnyCloseRequested()) {
    (void)processHttp1Requests(_connections.iterator(fd));
  }
}

void SingleHttpServer::deferInput(NativeHandle fd, ConnectionState& state) {
  if (!state.inputDeferred) {
    state.inputDeferred = true;
    _pendingReadFds.push_back(fd);
    _lifecycle.wakeupFd.send();
  }
}

SingleHttpServer::CloseStatus SingleHttpServer::resumeDeferredInput(ConnectionIt cnxIt) {
  const auto fd = cnxIt->fd();
  _connections.connectionState(cnxIt).inputDeferred = false;
  processBufferedInput(fd);
  ConnectionState& state = _connections.connectionState(_connections.iterator(fd));
  if (state.inputDeferred || state.isAnyCloseRequested()) {
    // Its responses used the fairness budget again, or it closes: nothing more to read now.
    return state.canCloseConnectionForDrain() ? CloseStatus::Close : CloseStatus::Keep;
  }
  return handleReadableClient(_connections.iterator(fd), false);
}

SingleHttpServer::CloseStatus SingleHttpServer::handleReadableClient(ConnectionIt cnxIt, bool stopOnShortRead) {
  ConnectionState* pCnx = _connections.pConnectionState(cnxIt);
  assert(pCnx != nullptr);

  // Pending output can legitimately be non-empty when we get EPOLLIN.
  // This happens with partial writes and very commonly with TLS (SSL_read/handshake progress
  // can generate outbound records that must be written before further progress).
  // Opportunistically flush here; if still blocked on write, yield and wait for EPOLLOUT.
  if (pCnx->hasPendingOutput()) {
    flushOutbound(cnxIt);
    if (pCnx->hasPendingOutput()) {
      if (!pCnx->waitingWritable) {
        enableWritableInterest(cnxIt);
      }
      return CloseStatus::Keep;
    }
  }

  // A tunnel endpoint relays raw bytes to its peer.
  if (pCnx->isTunneling() || pCnx->tunnelResolving) {
    return _tunnels->handleReadable(cnxIt);
  }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  if (pCnx->protocolHandler == nullptr && PauseReadingDuringAsyncHandler(*pCnx)) {
    return pCnx->canCloseConnectionForDrain() ? CloseStatus::Close : CloseStatus::Keep;
  }
#endif

  std::size_t bytesReadThisEvent = 0;
  // The bytes written while serving the input count toward the fairness budget too: a small request can have a large
  // response, so a client pipelining such requests (an HTTP/2 client keeping several streams busy, say) would otherwise
  // keep the event loop on its connection, while the other ones wait, until it has sent a whole budget of requests.
  const uint64_t bytesWrittenAtEventStart = totalBytesWritten();
  const auto fd = cnxIt->fd();
  while (true) {
    const std::size_t chunkSize = _config.computeReadChunkSize(bytesReadThisEvent);
    assert(chunkSize > 0);

    // Re-set the pointer on each loop iteration in case of connection state reallocations.
    cnxIt = _connections.iterator(fd);
    pCnx = _connections.pConnectionState(fd);

    const auto [count, want] = pCnx->transportRead(chunkSize);
    if (finalizeTlsHandshakeIfReady(fd, *pCnx)) {
      return CloseStatus::Close;
    }
    if (pCnx->waitingForBody && count > 0) {
      pCnx->bodyLastActivityMs = static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(_connections.now - pCnx->headerStartTp).count());
    }
    if (want == TransportHint::Error) [[unlikely]] {
#ifdef AERONET_ENABLE_OPENSSL
      CheckHandshake(_config.tls.enabled && _tls.ctxHolder, *pCnx, _tls.metrics, _callbacks.tlsHandshake, fd);
#endif
      return CloseStatus::Close;
    }
    if (want != TransportHint::None) {
      // Non-fatal: transport needs the socket to be readable or writable before proceeding.
      if (want == TransportHint::WriteReady && !pCnx->waitingWritable && !enableWritableInterest(cnxIt)) {
        return CloseStatus::Close;
      }
      break;
    }
    if (count == 0) {
#ifdef AERONET_ENABLE_OPENSSL
      CheckHandshake(_config.tls.enabled && _tls.ctxHolder, *pCnx, _tls.metrics, _callbacks.tlsHandshake, fd);
#endif
      // An orderly EOF only tells that the peer stopped sending: while the kernel still sends with zerocopy, close once
      // these sends completed instead of aborting them (see closeConnection()).
      if (pCnx->hasZerocopySendsInFlight()) {
        pCnx->requestDrainAndClose();
        return CloseStatus::Keep;
      }
      return CloseStatus::Close;
    }

    bytesReadThisEvent += static_cast<std::size_t>(count);

    // A plain socket read returning less than requested emptied the kernel receive queue. Edge-triggered polling
    // reports any byte arriving after it, so the next read would only return EAGAIN: skip that syscall.
    // Server TLS reads qualify too: they read ahead and decrypt records until the socket would block, unless the
    // transport reports pending input (a peer close_notify or an error to report after the returned data).
    const bool drained =
        stopOnShortRead && count < chunkSize &&
        (pCnx->transport.isPlain() || (pCnx->transport.isTls() && !pCnx->transport.hasPendingReadData()));

    if (processConnectionInput(cnxIt)) {
      break;
    }

    // CONNECT can switch this connection from HTTP parsing to raw tunneling inside
    // processConnectionInput(). The client can receive the 200 response and send its
    // first tunnel bytes before this edge-triggered read loop runs again. Continue in
    // tunnel mode immediately so those bytes are never fed back into the HTTP parser.
    cnxIt = _connections.iterator(fd);
    pCnx = _connections.pConnectionState(fd);
    if (pCnx->isTunneling()) {
      return _tunnels->relayInput(cnxIt);
    }

    // A drained read left nothing to read, and later data raises a new event: no need to defer the connection.
    if (!drained &&
        _config.fairnessBudgetExhausted(bytesReadThisEvent +
                                        static_cast<std::size_t>(totalBytesWritten() - bytesWrittenAtEventStart))) {
      // Edge-triggered polling (EPOLLET / EV_CLEAR): data may remain in the TCP buffer
      // after the fairness cap. No new read event fires on a non-empty→non-empty transition,
      // so defer this fd to read it again after the other ready connections have been served.
      deferInput(fd, *pCnx);
      break;
    }

    if (!pCnx->protocolHandler && pCnx->parsingHeaders && pCnx->inBuffer.size() > _config.maxHeaderBytes) {
      // Safety cap: prevent unbounded buffer growth while headers are still being received.
      // Checked after processConnectionInput so that a single read delivering headers + body together
      // does not falsely trigger: initTrySetHead clears parsingHeaders once the request head is parsed.
      // If we reach here, the header delimiter was not found and the buffer exceeds the header limit.
      emitSimpleError(cnxIt, http::StatusCodeRequestHeaderFieldsTooLarge, {});
      return CloseStatus::Close;
    }

    // Header read timeout enforcement: if headers of current pending request are not complete yet
    // (heuristic: no full request parsed and buffer not empty) and duration exceeded -> close.
    if (_config.headerReadTimeout.count() > 0 && pCnx->parsingHeaders &&
        _connections.now - pCnx->headerStartTp > _config.headerReadTimeout) {
      emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, {});
      return CloseStatus::Close;
    }

    if (drained) {
      break;
    }
  }
  // The input processing may have inserted connections.
  cnxIt = _connections.iterator(fd);
  pCnx = _connections.pConnectionState(fd);
  // Try to flush again after reading new data, in case TLS needed the read to proceed with write
  if (pCnx->hasPendingOutput()) {
    flushOutbound(cnxIt);
  }
  return pCnx->canCloseConnectionForDrain() ? CloseStatus::Close : CloseStatus::Keep;
}

}  // namespace aeronet
