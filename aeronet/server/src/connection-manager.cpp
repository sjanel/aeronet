#ifdef AERONET_WINDOWS
#include <ws2tcpip.h>
#else
#include <sys/socket.h>  // sockaddr_storage
#endif

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/accept-callouts.hpp"
#include "aeronet/base-fd.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/io-callouts.hpp"
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

#ifdef AERONET_ENABLE_TEST_HOOKS
#include "aeronet/transport-test-hook.hpp"
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

void SingleHttpServer::refreshKeepAliveDeadline(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  if (!_config.enableKeepAlive || state.isTunneling()) {
    _keepAliveDeadlines.remove(state);
    return;
  }
  _keepAliveDeadlines.upsert(state, cnxIt->fd(), state.lastActivity + _config.keepAliveTimeout);
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
    if (!_config.enableKeepAlive || state.isTunneling()) {
      continue;
    }

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
  if (!_config.enableKeepAlive) {
    return;
  }

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
  // Async-recv connections track writability via a one-shot poll and never increment
  // writableConnections (see enableWritableInterest), so there is nothing to decrement here.
  // usesAsyncRecv is always false on non-io_uring builds, so this is a no-op there.
  if (state.usesAsyncRecv) {
    return;
  }
  assert(_connectionSweepState.writableConnections > 0U);
  --_connectionSweepState.writableConnections;
}

void SingleHttpServer::forgetConnectionMaintenance(ConnectionState& state) {
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
#ifdef AERONET_IO_URING
  if (_connectionSweepState.parkedConnections != 0U) {
    return true;
  }
#endif
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

#ifdef AERONET_IO_URING
    // Parked connection: closed from the server's point of view, waiting for the kernel's
    // terminal recv/send CQEs before its buffers can be released.  Give an in-flight send
    // one full sweep interval to deliver (e.g. a 408 emitted right before the close); on the
    // second visit force-abort it so a peer that stopped reading cannot pin the buffers.
    if (state.closePendingAsyncCqe) {
      if (state.closeParkSweepSeen) {
        ShutdownReadWrite(fd);
      } else {
        state.closeParkSweepSeen = true;
      }
      continue;
    }
#endif

    // Retry pending file sends to handle potential missed EPOLLOUT edges.
    if (state.isSendingFile() && state.waitingWritable && flushRetries < kMaxFlushRetriesPerSweep) {
      flushFilePayload(cnxIt);
      ++flushRetries;
    }
    // Retry pending outbound buffer flushes to handle potential missed EPOLLOUT edges.
    // On Windows, WSAPoll can fail to report writability on loopback sockets, leaving
    // buffered response data (including HTTP/2 DATA frames) stuck in outBuffer indefinitely.
    // Periodic retry here ensures forward progress regardless of missed poll events.
    if (!state.outBuffer.empty() && state.waitingWritable && flushRetries < kMaxFlushRetriesPerSweep) {
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

#ifdef AERONET_ENABLE_HTTP2
    // Sweep per-stream request deadlines for HTTP/2 connections.
    if (state.protocol == ProtocolType::Http2 && state.protocolHandler) {
      static_cast<http2::Http2ProtocolHandler*>(state.protocolHandler.get())->sweepStreams(now);
    }
#endif

    // Skip reclaim while an async recv or send is in flight: the kernel holds pointers into
    // inBuffer / outBuffer and a realloc/shrink would move or shrink the buffer beneath it,
    // leading to heap-buffer-overflow when the CQE arrives.
    if (!state.asyncRecvInFlight && !state.asyncSendInFlight) {
      state.reclaimMemoryFromOversizedBuffers();
    }

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
  for (decltype(_config.maxAcceptBatchSize) accepted = 0; accepted < maxAcceptBatch; ++accepted) {
    sockaddr_storage peerAddress;
    Connection cnx(_listenSocket, peerAddress);
    if (!cnx) {
      // no more waiting connections
      break;
    }
    setupAcceptedConnection(std::move(cnx), peerAddress);
  }
}

void SingleHttpServer::setupAcceptedConnection(Connection cnx, const sockaddr_storage& peerAddress) {
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
  // Decide read strategy up-front so we don't waste a poll registration we'd immediately tear down.
  // Async-recv (io_uring proactor) is used for non-TLS connections to avoid per-event read syscalls.
  // AeronetUseIoRingForFd lets test binaries force the synchronous transport path per-fd.
#if defined(AERONET_IO_URING) && defined(AERONET_ENABLE_OPENSSL)
  const bool wantsAsyncRecv = !_tls.ctxHolder && AeronetUseIoRingForFd(cnxFd);
#elif defined(AERONET_IO_URING)
  const bool wantsAsyncRecv = AeronetUseIoRingForFd(cnxFd);
#else
  constexpr bool wantsAsyncRecv = false;
#endif
  if (!wantsAsyncRecv) {
    if (!_eventLoop.add(EventLoop::EventFd{cnxFd, EventIn | EventRdHup | EventEt})) [[unlikely]] {
      _telemetry.counterAdd("aeronet.connections.errors.add_event_failed", 1UL);
      return;
    }
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
      return;
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
        return;
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
      FailTlsHandshakeOnce(state, _tls.metrics, _callbacks.tlsHandshake, cnxFd, kTlsHandshakeFailureReasonSslNewFailed);
      closeConnection(cnxIt);
      return;
    }

    // Install per-connection observer for OpenSSL callbacks.
    if (SetTlsHandshakeObserver(reinterpret_cast<ssl_st*>(sslPtr.get()), &state.tlsHandshakeObserver) != 1)
        [[unlikely]] {
      log::error("SSL_set_ex_data failed to install TLS handshake observer for fd # {}", cnxFd);
      // Treat this as a handshake failure: record metrics, emit event, and close the connection.
      FailTlsHandshakeOnce(state, _tls.metrics, _callbacks.tlsHandshake, cnxFd,
                           kTlsHandshakeFailureReasonSetExDataFailed);
      closeConnection(cnxIt);
      return;
    }

    // OpenSSL's SSL_set_fd takes int; on Windows SOCKET is UINT_PTR but the value
    // round-trips safely through int for sockets allocated by the OS.
    if (AeronetSslSetFd(sslPtr.get(), static_cast<int>(cnxFd)) != 1) [[unlikely]] {  // associate
      log::error("SSL_set_fd failed for fd # {}", cnxFd);
      FailTlsHandshakeOnce(state, _tls.metrics, _callbacks.tlsHandshake, cnxFd,
                           kTlsHandshakeFailureReasonSslSetFdFailed);
      closeConnection(cnxIt);
      return;
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

#ifdef AERONET_ENABLE_TEST_HOOKS
  // Test hook: allow tests to wrap/decorate the transport for fault injection.
  state.transport = test::ApplyTransportDecorator(std::move(state.transport));
#endif

  refreshKeepAliveDeadline(cnxIt);

#ifdef AERONET_IO_URING
  // ----- Async recv (io_uring proactor) path -----
  // For non-TLS connections, submit a single-shot async recv SQE: data lands directly in
  // inBuffer with a single CQE round-trip per chunk and no per-event read syscall.  TLS
  // keeps the synchronous read loop because OpenSSL must drive the handshake state machine.
  if (wantsAsyncRecv) {
    const std::size_t chunkSize = _config.computeReadChunkSize(0);
    state.inBuffer.ensureAvailableCapacityExponential(chunkSize);
    const auto avail = state.inBuffer.capacity() - state.inBuffer.size();
    if (_eventLoop.submitRecv(cnxFd, state.inBuffer.data() + state.inBuffer.size(), avail)) [[likely]] {
      state.usesAsyncRecv = true;
      state.asyncRecvInFlight = true;
      return;
    }
    // Fallback: register multishot poll and continue with the sync read loop.
    if (!_eventLoop.add(EventLoop::EventFd{cnxFd, EventIn | EventRdHup | EventEt})) [[unlikely]] {
      _telemetry.counterAdd("aeronet.connections.errors.add_event_failed", 1UL);
      closeConnection(cnxIt);
      return;
    }
  }
#endif

  ConnectionState* pCnx = &state;
  std::size_t bytesReadThisEvent = 0;
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
      CheckHandshake(_config.tls.enabled && _tls.ctxHolder, *pCnx, _tls.metrics, _callbacks.tlsHandshake, cnxFd, want);
#endif
      closeConnection(cnxIt);
      pCnx = nullptr;
      break;
    }
    if (want != TransportHint::None) {
      // Transport indicates we should wait for readability or writability before continuing.
      // Adjust epoll interest if TLS handshake needs write readiness
      if (want == TransportHint::WriteReady && !pCnx->waitingWritable) {
        if (!enableWritableInterest(cnxIt)) [[unlikely]] {
          closeConnection(cnxIt);
          pCnx = nullptr;
        }
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
    if (_config.fairnessBudgetExhausted(bytesReadThisEvent)) {
      break;
    }
  }
  if (pCnx == nullptr) {
    return;
  }

  const bool closeNow = processConnectionInput(cnxIt);
  if (closeNow && pCnx->outBuffer.empty() && pCnx->tunnelOrFileBuffer.empty() && !pCnx->isSendingFile()) {
    closeConnection(cnxIt);
  }
}

void SingleHttpServer::closeConnection(ConnectionIt cnxIt) {
  const auto cfd = cnxIt->fd();
  ConnectionState& state = _connections.connectionState(cnxIt);
  if (state.closePendingAsyncCqe) {
    // Already closed from the server's point of view; the storage slot is parked until the
    // kernel's terminal CQEs are harvested (finishParkedConnectionCqe completes the release).
    return;
  }
  log::debug("closeConnection called for fd # {}", cfd);
  forgetConnectionMaintenance(state);

  // If this is a tunnel endpoint (CONNECT), ensure we tear down the peer too.
  // Otherwise, peerFd may dangle and later accidentally match a reused fd, causing
  // spurious epoll_ctl failures and incorrect forwarding.
  const auto peerFd = state.peerFd;
  if (peerFd != kInvalidHandle) {
    auto peerIt = _connections.iterator(peerFd);
    if (IsValid(_connections, peerIt)) [[likely]] {
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
        forgetConnectionMaintenance(peerConnectionState);
        releaseConnection(peerIt);
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

  releaseConnection(cnxIt);

#ifdef AERONET_ENABLE_HTTP2
  // Close tunnel upstream fds after the HTTP/2 connection has been released.
  // Set peerFd = -1 on each to prevent them from trying to close the already-released peer.
  for (const auto& [upFd, streamId] : tunnelUpstreamFds) {
    auto upIt = _connections.iterator(upFd);
    if (IsValid(_connections, upIt)) {
      ConnectionState& upState = _connections.connectionState(upIt);
      upState.peerFd = kInvalidHandle;
      upState.peerStreamId = 0;
      forgetConnectionMaintenance(upState);
      releaseConnection(upIt);
    }
  }
#endif
}

void SingleHttpServer::releaseConnection(ConnectionIt cnxIt) {
  const auto fd = cnxIt->fd();
#ifdef AERONET_IO_URING
  ConnectionState& state = _connections.connectionState(cnxIt);
  if (state.asyncRecvInFlight || state.asyncSendInFlight) {
    // The kernel still holds pointers into inBuffer / outBuffer.  Releasing the storage now
    // would let those buffers be freed or reused while the kernel reads or writes them.
    // Park the slot and shut down the read half only: the pending recv completes with EOF at
    // the next ring enter, while an in-flight send (e.g. a 408 emitted right before a sweep
    // close) still delivers its bytes — finishParkedConnectionCqe keeps draining queued
    // output and completes the release when the terminal CQEs are harvested.  A client that
    // stops reading is force-aborted by the sweep after one grace tick (closeParkSweepSeen).
    state.closePendingAsyncCqe = true;
    state.closeParkSweepSeen = false;
    ++_connectionSweepState.parkedConnections;
    state.peerFd = kInvalidHandle;
    state.peerStreamId = 0;
    if (!ShutdownRead(fd)) {
      log::debug("shutdown failed for parked fd # {} (already reset by peer?)", fd);
    }
    return;
  }
#endif
  _eventLoop.del(fd);
#ifdef AERONET_ENABLE_OPENSSL
  _connections.recycleOrRelease(cnxIt, _config.maxCachedConnections, _config.tls.enabled, _tls.handshakesInFlight);
#else
  _connections.recycleOrRelease(cnxIt, _config.maxCachedConnections);
#endif
  _eventLoop.submitClose(fd);
}

#ifdef AERONET_IO_URING
void SingleHttpServer::drainRingAtLoopExit() {
  _eventLoop.cancelAllOps();
  for (int round = 0; round < 8; ++round) {
    const auto events = _eventLoop.poll();
    if (events.empty()) {
      break;  // cancellations flushed and drained
    }
    for (const auto event : events) {
      if ((event.eventBmp & EventAccept) != 0) {
        // A connection raced in before the accept cancellation took effect.  The listener
        // stays open across restarts, so this connection belongs to the next run: register
        // it normally (its recv arms now and completes under the restarted loop).
        AeronetOnConnectionAccepted(event.fd);
        sockaddr_storage peerAddress{};
        GetPeerAddress(event.fd, peerAddress);
        setupAcceptedConnection(Connection(BaseFd(event.fd)), peerAddress);
        continue;
      }
      if ((event.eventBmp & (EventDataArrived | EventSendComplete)) == 0) {
        continue;  // cancelled multishot polls / poll-once — nothing to hand back
      }
      auto cnxIt = _connections.iterator(event.fd);
      if (!IsValid(_connections, cnxIt)) {
        continue;
      }
      if (finishParkedConnectionCqe(cnxIt, event.eventBmp, event.bytesAvailable)) {
        continue;
      }
      // Live connection: run the normal completion handlers so no request data or queued
      // output is lost across a restart.  A request completing here queues its response via
      // an async send whose CQE is consumed by a later drain round; cancelled recvs
      // (-ECANCELED) close the connection cleanly (FIN) instead of leaving it deaf.
      CloseStatus closeStatus = CloseStatus::Keep;
      if ((event.eventBmp & EventSendComplete) != 0) {
        closeStatus = handleAsyncSendCompletion(cnxIt, event.bytesAvailable);
      }
      if ((event.eventBmp & EventDataArrived) != 0) {
        cnxIt = _connections.iterator(event.fd);
        if (!IsValid(_connections, cnxIt)) [[unlikely]] {
          continue;
        }
        closeStatus = std::max(handleAsyncRecvCompletion(cnxIt, event.bytesAvailable), closeStatus);
      }
      if (closeStatus == CloseStatus::Close) {
        const auto finalIt = _connections.iterator(event.fd);
        if (IsValid(_connections, finalIt)) {
          closeConnection(finalIt);
        }
      }
    }
  }
}

bool SingleHttpServer::finishParkedConnectionCqe(ConnectionIt cnxIt, EventBmp bmp, int32_t bytesAvailable) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  if (!state.closePendingAsyncCqe) {
    return false;
  }
  if ((bmp & EventDataArrived) != 0) {
    state.asyncRecvInFlight = false;
  }
  if ((bmp & EventSendComplete) != 0) {
    state.asyncSendInFlight = false;
    if (bytesAvailable > 0) {
      // Keep delivering the output that was queued before the close (drain-then-close
      // semantics survive parking): advance past the sent span and submit the next one.
      _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(bytesAvailable);
      state.outBuffer.addOffset(static_cast<std::size_t>(bytesAvailable));
      for (;;) {
        if (!state.outBuffer.empty() && startAsyncSend(cnxIt)) {
          break;  // next span in flight — stay parked
        }
        state.outBuffer.clear();
        if (state.pendingOutBuffer.empty()) {
          break;
        }
        state.outBuffer = std::move(state.pendingOutBuffer);
        state.pendingOutBuffer.clear();
      }
    } else {
      // Send failed (peer gone / force-aborted by the sweep): drop remaining output.
      state.outBuffer.clear();
      state.pendingOutBuffer.clear();
    }
  }
  if (!state.asyncRecvInFlight && !state.asyncSendInFlight) {
    state.closePendingAsyncCqe = false;
    assert(_connectionSweepState.parkedConnections > 0U);
    --_connectionSweepState.parkedConnections;
    releaseConnection(cnxIt);
  }
  return true;
}
#endif

bool SingleHttpServer::finalizeTlsHandshakeIfReady([[maybe_unused]] NativeHandle fd, ConnectionState& state) {
  if (state.tlsEstablished || !state.transport->handshakeDone()) {
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

  // If this connection was created for an upstream non-blocking connect, and connect is pending,
  // check SO_ERROR to determine whether connect completed successfully or failed.
  const auto fd = cnxIt->fd();
  if (state.connectPending) {
    const int err = GetSocketError(fd);
    state.connectPending = false;
    if (err != 0) {
      // Upstream connect failed. Attempt to notify the client side (peerFd) and close this upstream.
      const auto peerIt = _connections.iterator(state.peerFd);
      if (IsValid(_connections, peerIt)) {
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

#ifdef AERONET_IO_URING
  // An async recv SQE owns the inBuffer tail (one-shot writability polls can surface
  // POLLERR/POLLHUP here for async-recv connections, e.g. during file sends).  A synchronous
  // read would deposit bytes at the same tail position the kernel is about to write to.
  // EOF/errors surface through the pending recv CQE instead.
  if (pCnx->asyncRecvInFlight) {
    return CloseStatus::Keep;
  }
#endif

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
      if (want == TransportHint::WriteReady && !pCnx->waitingWritable) {
        if (!enableWritableInterest(cnxIt)) [[unlikely]] {
          return CloseStatus::Close;
        }
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

    if (processConnectionInput(cnxIt)) {
      break;
    }

    if (_config.fairnessBudgetExhausted(bytesReadThisEvent)) {
      // Edge-triggered polling (EPOLLET / EV_CLEAR): data may remain in the TCP buffer
      // after the fairness cap. No new read event fires on a non-empty→non-empty transition,
      // so defer this fd for re-read at the start of the next event-loop iteration.
      _pendingReadFds.push_back(fd);
      _lifecycle.wakeupFd.send();
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
    if (_config.headerReadTimeout.count() > 0 && pCnx->parsingHeaders) {
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

bool SingleHttpServer::armAsyncRecv(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  assert(state.usesAsyncRecv);
  assert(!state.asyncRecvInFlight);
  const auto fd = cnxIt->fd();
  const std::size_t chunkSize = _config.computeReadChunkSize(0);
  state.inBuffer.ensureAvailableCapacityExponential(chunkSize);
  const auto avail = state.inBuffer.capacity() - state.inBuffer.size();
  if (avail == 0) [[unlikely]] {
    // Buffer cap reached; drop async path so backpressure / sync path can decide.
    state.usesAsyncRecv = false;
    if (!_eventLoop.add(EventLoop::EventFd{fd, EventIn | EventRdHup | EventEt})) [[unlikely]] {
      return false;
    }
    return true;
  }
  if (AeronetUseIoRingForFd(fd) && _eventLoop.submitRecv(fd, state.inBuffer.data() + state.inBuffer.size(), avail))
      [[likely]] {
    state.asyncRecvInFlight = true;
    return true;
  }
  // SQ full, backend unsupported, or ring I/O disabled for this fd (test error injection):
  // fall back to multishot poll for this connection.
  state.usesAsyncRecv = false;
  if (!_eventLoop.add(EventLoop::EventFd{fd, EventIn | EventRdHup | EventEt})) [[unlikely]] {
    return false;
  }
  return true;
}

bool SingleHttpServer::startAsyncSend([[maybe_unused]] ConnectionIt cnxIt) {
#ifdef AERONET_IO_URING
  ConnectionState& state = _connections.connectionState(cnxIt);
  assert(!state.asyncSendInFlight);
  const std::string_view first = state.outBuffer.firstBuffer();
  const std::string_view span = first.empty() ? state.outBuffer.secondBuffer() : first;
  if (span.empty()) {
    return true;  // nothing left to send
  }
  // MSG_MORE when the head span is followed by a body span: the kernel holds the partial
  // segment until the next send, coalescing head + body without TCP_CORK syscalls.
  const bool moreToCome = !first.empty() && !state.outBuffer.secondBuffer().empty();
  if (_eventLoop.submitSend(cnxIt->fd(), span.data(), span.size(), moreToCome)) [[likely]] {
    state.asyncSendInFlight = true;
    return true;
  }
  return false;
#else
  return false;  // async sends are only started on io_uring builds
#endif
}

SingleHttpServer::CloseStatus SingleHttpServer::handleAsyncSendCompletion(ConnectionIt cnxIt, int32_t sentBytes) {
#ifdef AERONET_IO_URING
  ConnectionState& state = _connections.connectionState(cnxIt);
  assert(state.asyncSendInFlight);
  state.asyncSendInFlight = false;

  if (sentBytes < 0) [[unlikely]] {
    if (sentBytes == -EINTR || sentBytes == -EAGAIN) {
      // Spurious wakeup — resubmit the unsent remainder.
      if (!startAsyncSend(cnxIt)) {
        flushOutbound(cnxIt);
      }
      return state.canCloseConnectionForDrain() ? CloseStatus::Close : CloseStatus::Keep;
    }
    // Fatal send error (EPIPE / ECONNRESET / ...): drop all queued output and close.
    log::debug("io_uring send failed (fd # {}, err={})", cnxIt->fd(), -sentBytes);
    state.outBuffer.clear();
    state.pendingOutBuffer.clear();
    state.requestDrainAndClose();
    return CloseStatus::Close;
  }

  // Async sends are the primary write path (the ring replaces queueData's immediate write).
  _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(sentBytes);
  state.outBuffer.addOffset(static_cast<std::size_t>(sentBytes));

  // Continue sending until a new SQE is in flight or all queued output is drained.
  for (;;) {
    if (!state.outBuffer.empty()) {
      if (startAsyncSend(cnxIt)) [[likely]] {
        return CloseStatus::Keep;  // next span in flight
      }
      // SQ exhausted (rare): drain synchronously to preserve ordering.
      flushOutbound(cnxIt);
      if (!state.outBuffer.empty()) {
        return state.canCloseConnectionForDrain() ? CloseStatus::Close : CloseStatus::Keep;
      }
    }
    state.outBuffer.clear();  // release consumed spans + reset offset
    if (state.pendingOutBuffer.empty()) {
      break;
    }
    // Promote responses staged while the previous send was in flight.
    state.outBuffer = std::move(state.pendingOutBuffer);
    state.pendingOutBuffer.clear();
  }

  // All outbound data drained.
  if (state.fileSendHeadersPending) {
    state.fileSendHeadersPending = false;
  }
  if (state.isSendingFile()) {
    flushFilePayload(cnxIt);
  }
  if (state.canCloseConnectionForDrain()) {
    return CloseStatus::Close;
  }

  // Pipelined requests already buffered in inBuffer were deferred while outBuffer was
  // non-empty (processHttp1Requests gate) — resume them now. HTTP/1 only: protocol
  // handlers (WebSocket / HTTP/2) never defer input on outBuffer, so their leftover
  // inBuffer bytes are always an incomplete frame — re-parsing them on every send
  // completion would be pure waste on the per-frame hot path.
  if (!state.inBuffer.empty() && state.protocolHandler == nullptr && !state.isTunneling()) {
    const auto fd = cnxIt->fd();
    if (processConnectionInput(cnxIt)) {
      // processConnectionInput may have invalidated the iterator; re-resolve.
      cnxIt = _connections.iterator(fd);
      if (!IsValid(_connections, cnxIt)) [[unlikely]] {
        return CloseStatus::Close;
      }
      ConnectionState* pCnx = _connections.pConnectionState(cnxIt);
      if (pCnx->outBuffer.empty() && pCnx->tunnelOrFileBuffer.empty() && !pCnx->isSendingFile()) {
        return CloseStatus::Close;
      }
      pCnx->requestDrainAndClose();
    }
  }
  return CloseStatus::Keep;
#else
  (void)cnxIt;
  (void)sentBytes;
  return CloseStatus::Close;  // unreachable — send CQEs only exist on io_uring builds
#endif
}

SingleHttpServer::CloseStatus SingleHttpServer::handleAsyncRecvCompletion(ConnectionIt cnxIt, int32_t bytesAvailable) {
  ConnectionState* pCnx = _connections.pConnectionState(cnxIt);
  assert(pCnx != nullptr);
  assert(pCnx->usesAsyncRecv);
  pCnx->asyncRecvInFlight = false;

  if (bytesAvailable <= 0) {
    if (bytesAvailable == -EINTR || bytesAvailable == -EAGAIN) [[unlikely]] {
      // Spurious wakeup — re-arm the recv.
      return armAsyncRecv(cnxIt) ? CloseStatus::Keep : CloseStatus::Close;
    }
    // 0 = orderly EOF; other negatives (-ECONNRESET, ...) are terminal errors.
    pCnx->eofReceived = true;
#ifdef AERONET_ENABLE_OPENSSL
    CheckHandshake(_config.tls.enabled && _tls.ctxHolder, *pCnx, _tls.metrics, _callbacks.tlsHandshake, cnxIt->fd());
#endif
    return CloseStatus::Close;
  }

  // The kernel deposited bytesAvailable bytes at inBuffer.data() + inBuffer.size()
  // (the tail location captured at the time of submitRecv()).  Advance the size to
  // make those bytes visible to the parser.
  pCnx->inBuffer.addSize(static_cast<std::size_t>(bytesAvailable));
  _telemetry.counterAdd("aeronet.bytes.read", static_cast<uint64_t>(bytesAvailable));

  // Mirror what transportRead() does on first byte arrival: stamp the header-read deadline
  // so sweepIdleConnections can enforce header read timeouts on async-recv connections.
  // The matching ++pendingTimeoutConnections mirrors the synchronous read loop above so the
  // header-parse-complete decrement in processHttp1Requests stays balanced.
  if (pCnx->headerStartTp.time_since_epoch().count() == 0) {
    pCnx->headerStartTp = pCnx->lastActivity;
    pCnx->parsingHeaders = true;
    if (_config.headerReadTimeout.count() > 0) {
      ++_connectionSweepState.pendingTimeoutConnections;
    }
  }

  if (pCnx->waitingForBody) {
    pCnx->bodyLastActivityMs = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(_connections.now - pCnx->headerStartTp).count());
  }

  const auto fd = cnxIt->fd();
  if (processConnectionInput(cnxIt)) {
    // processConnectionInput may have invalidated the iterator; re-resolve.
    cnxIt = _connections.iterator(fd);
    if (!IsValid(_connections, cnxIt)) [[unlikely]] {
      return CloseStatus::Close;
    }
    pCnx = _connections.pConnectionState(cnxIt);
    assert(pCnx != nullptr);
    // Mirror handleReadableClient's drain-then-close: only close immediately when
    // there is no pending outbound data or active file send.  Otherwise wait for
    // the writable poll / file payload to flush before tearing down.
    if (pCnx->outBuffer.empty() && pCnx->tunnelOrFileBuffer.empty() && !pCnx->isSendingFile()) {
      return CloseStatus::Close;
    }
    // Continue to re-arm recv (or skip if drain pending) so further writable events
    // can drain the response.  Mark drain so canCloseConnectionForDrain can finalize later.
    pCnx->requestDrainAndClose();
  }

  // processConnectionInput may have invalidated the iterator; re-resolve.
  cnxIt = _connections.iterator(fd);
  if (!IsValid(_connections, cnxIt)) [[unlikely]] {
    return CloseStatus::Close;
  }
  pCnx = _connections.pConnectionState(cnxIt);
  assert(pCnx != nullptr);

  if (!pCnx->protocolHandler && pCnx->parsingHeaders && pCnx->inBuffer.size() > _config.maxHeaderBytes) {
    emitSimpleError(cnxIt, http::StatusCodeRequestHeaderFieldsTooLarge, {});
    return CloseStatus::Close;
  }
  if (_config.headerReadTimeout.count() > 0 && pCnx->parsingHeaders) {
    if (_connections.now - pCnx->headerStartTp > _config.headerReadTimeout) {
      emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, {});
      return CloseStatus::Close;
    }
  }

  if (pCnx->canCloseConnectionForDrain()) {
    return CloseStatus::Close;
  }

  // Tunneling transitions: forwarding loop expects synchronous reads via transport.
  // Drop async-recv and fall back to multishot poll for this connection.
  if (pCnx->isTunneling()) {
    pCnx->usesAsyncRecv = false;
    if (!_eventLoop.add(EventLoop::EventFd{fd, EventIn | EventRdHup | EventEt})) [[unlikely]] {
      return CloseStatus::Close;
    }
    return CloseStatus::Keep;
  }

  if (!armAsyncRecv(cnxIt)) {
    return CloseStatus::Close;
  }
  return CloseStatus::Keep;
}

// ============================================================================
// Shared CONNECT tunnel helpers (HTTP/1.1 + HTTP/2)
// ============================================================================

NativeHandle SingleHttpServer::setupTunnelConnection(NativeHandle clientFd, std::string_view host, uint16_t port) {
  ConnectResult cres = ConnectTCP(std::span<char>(const_cast<char*>(host.data()), host.size()), port);
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
  const auto upIt = _connections.emplace(std::move(cres.cnx));
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
    if (!target.waitingWritable && !enableWritableInterest(targetIt)) [[unlikely]] {
      return false;
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
    if (!target.waitingWritable && !enableWritableInterest(targetIt)) [[unlikely]] {
      return false;
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
    if (!target.waitingWritable && !enableWritableInterest(targetIt)) [[unlikely]] {
      return false;
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
    if (!target.waitingWritable && !enableWritableInterest(targetIt)) [[unlikely]] {
      return false;
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
    const std::size_t chunkSize = _config.computeReadChunkSize(bytesReadThisEvent);
    assert(chunkSize > 0);
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
    if (_config.fairnessBudgetExhausted(bytesReadThisEvent)) {
      // Edge-triggered polling (EPOLLET): data may remain in the TCP buffer after the fairness cap.
      // No new read event fires on a non-empty→non-empty transition, so defer this fd for
      // re-processing at the start of the next event-loop iteration (same as handleReadableClient).
      _pendingReadFds.push_back(cnxIt->fd());
      _lifecycle.wakeupFd.send();
      hitEagain = true;
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
      if (IsValid(_connections, peerIt)) {
        if (shutdownTunnelPeerWrite(peerIt)) {
          return CloseStatus::Keep;  // peer closed and cleaned up
        }
      }
      // Stop reading from this side — wait for the peer to close or drain.
      if (!_eventLoop.mod(EventLoop::EventFd{selfFd, EventOut | EventRdHup | EventEt})) [[unlikely]] {
        return CloseStatus::Close;
      }
    }
    return CloseStatus::Keep;
  }

  auto peerIt = _connections.iterator(state.peerFd);
  if (!IsValid(_connections, peerIt)) [[unlikely]] {
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
    if (!_eventLoop.mod(EventLoop::EventFd{selfFd, EventOut | EventRdHup | EventEt})) [[unlikely]] {
      return CloseStatus::Close;
    }
  }
  return CloseStatus::Keep;
}

}  // namespace aeronet
