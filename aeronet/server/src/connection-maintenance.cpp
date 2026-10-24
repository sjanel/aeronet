#include "aeronet/internal/connection-maintenance.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <string_view>

#include "aeronet/connection-state.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/keep-alive-deadline-queue.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/system-error.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-protocol-handler.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-handshake-failure-reasons.hpp"
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-handler.hpp"
#endif

namespace aeronet::internal {

void ConnectionMaintenance::refreshKeepAliveDeadline(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  const HttpServerConfig& config = ctx.config;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
#ifdef AERONET_ENABLE_WEBSOCKET
  // keepAliveTimeout bounds the idleness between HTTP requests: an upgraded connection has its own timeouts.
  if (state.protocol == ProtocolType::WebSocket) {
    refreshWebSocketDeadline(cnxIt->fd(), state);
    return;
  }
#endif
  // A lingering close keeps the deadline armed when it started (re-armed from the last activity after a rebuild).
  if (state.isLingering()) {
    armDeadlineIfUnset(state, cnxIt->fd(), state.lastActivity + config.lingeringCloseTimeout);
    return;
  }
  // A CONNECT waiting for its target resolution has a deadline of its own (TunnelManager::expirePendingTunnels()).
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  const auto* asyncState = state.pAsyncState();
  bool asyncHandlerActive = asyncState != nullptr && asyncState->active;
  if (!config.enableKeepAlive || state.isTunneling() || state.tunnelResolving || asyncHandlerActive) {
#else
  if (!config.enableKeepAlive || state.isTunneling() || state.tunnelResolving) {
#endif
#ifdef AERONET_LINUX
    // Still watched while its zerocopy sends are in flight (see watchZerocopySends()).
    if (state.hasZerocopySendsInFlight()) {
      armDeadlineIfUnset(state, cnxIt->fd(), state.lastActivity + config.keepAliveTimeout);
      return;
    }
#endif
    _deadlines.remove(state);
    return;
  }
  // An armed deadline is never later than lastActivity + keepAliveTimeout: lastActivity only moves forward, and a
  // configuration update rebuilds the queue. Keep it: closeExpiredKeepAliveConnections() re-arms it from lastActivity
  // when it fires, so that an active connection costs one heap update per keepAliveTimeout instead of one per event.
  armDeadlineIfUnset(state, cnxIt->fd(), state.lastActivity + config.keepAliveTimeout);
}

void ConnectionMaintenance::armDeadlineIfUnset(ConnectionState& state, NativeHandle fd,
                                               std::chrono::steady_clock::time_point expiresAt) {
  if (!KeepAliveDeadlineQueue::contains(state)) {
    _deadlines.upsert(state, fd, expiresAt);
  }
}

void ConnectionMaintenance::restartKeepAliveIdleWindow(NativeHandle fd, std::chrono::steady_clock::time_point workEnd) {
  ServerContext& ctx = *_pCtx;
  if (!ctx.config.enableKeepAlive) {
    return;
  }
  const auto cnxIt = ctx.connections.iterator(fd);
  if (!IsValid(ctx.connections, cnxIt)) {
    return;
  }
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  // The deadline armed when the request was read is stale as soon as serving it took longer than
  // keepAliveTimeout - a multi-second database query in a synchronous handler is enough. keepAliveTimeout
  // bounds idleness *between* requests (see HttpServerConfig::keepAliveTimeout), so restart the idle window
  // from the instant the work completed. Without this the next maintenance sweep would close the connection
  // right after the response was produced, before the peer had any chance to read it.
  if (state.lastActivity + ctx.config.keepAliveTimeout > workEnd) {
    return;  // served well within the idle window: the armed deadline is still valid
  }
  state.lastActivity = workEnd;
  refreshKeepAliveDeadline(cnxIt);
}

bool ConnectionMaintenance::closeExpiredKeepAliveConnections() {
  ServerContext& ctx = *_pCtx;
  const HttpServerConfig& config = ctx.config;
  bool closedAny = false;
  const auto now = ctx.connections.now;

  while (!_deadlines.empty()) {
    const auto& next = _deadlines.top();
    if (next.expiresAt > now) {
      break;
    }

    const auto expired = _deadlines.pop();
    auto cnxIt = ctx.connections.iterator(expired.fd);
    if (!IsValid(ctx.connections, cnxIt) || ctx.connections.pConnectionState(cnxIt) != expired.pState) [[unlikely]] {
      continue;
    }

    ConnectionState& state = ctx.connections.connectionState(cnxIt);
    if (state.isLingering()) {
      log::debug("sweepIdleConnections: fd # {} closed for lingering close timeout", expired.fd);
      ctx.io.closeConnectionNow(cnxIt);
      ctx.telemetry.counterAdd("aeronet.connections.closed_for_lingering_close_timeout");
      closedAny = true;
      continue;
    }
#ifdef AERONET_ENABLE_WEBSOCKET
    if (state.protocol == ProtocolType::WebSocket) {
      closedAny |= checkWebSocketTimeouts(cnxIt);
      continue;
    }
#endif
#ifdef AERONET_LINUX
    if (state.hasZerocopySendsInFlight()) {
      closedAny |= watchZerocopySends(cnxIt);
      continue;
    }
#endif
    if (!config.enableKeepAlive || state.isTunneling() || state.tunnelResolving) {
      continue;
    }

#ifdef AERONET_ENABLE_HTTP2
    // keepAliveTimeout applies between requests, after the previous response has completed. An active HTTP/2
    // stream may be waiting for peer flow-control credit and therefore produce no socket events at all; reaping
    // it here would truncate a response at the current window boundary. Recheck periodically until all streams
    // complete, at which point the last event-refreshed deadline resumes normal idle expiry.
    if (state.protocolHandler && state.protocolHandler->type() == ProtocolType::Http2 &&
        static_cast<http2::Http2ProtocolHandler*>(state.protocolHandler.get())->connection().activeStreamCount() != 0) {
      _deadlines.upsert(state, expired.fd, now + config.keepAliveTimeout);
      continue;
    }
#endif

    // File sends are exempt because their progress is driven by this very sweep (flushFilePayload retries
    // above), which does not refresh lastActivity.
    if (state.isSendingFile()) {
      _deadlines.upsert(state, expired.fd, now + config.keepAliveTimeout);
      continue;
    }

    const auto currentExpiry = state.lastActivity + config.keepAliveTimeout;
    if (currentExpiry > now) {
      _deadlines.upsert(state, expired.fd, currentExpiry);
      continue;
    }

    log::debug("sweepIdleConnections: fd # {} closed for keep-alive timeout", expired.fd);
    ctx.io.closeConnection(cnxIt);
    ctx.telemetry.counterAdd("aeronet.connections.closed_for_keep_alive");
    closedAny = true;
  }

  return closedAny;
}

#ifdef AERONET_LINUX
bool ConnectionMaintenance::watchZerocopySends(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  const HttpServerConfig& config = ctx.config;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  const NativeHandle fd = cnxIt->fd();
  const auto now = ctx.connections.now;

  // The completions are read on EPOLLERR: the sends were in flight at the last event of the connection.
  const auto activityExpiry = state.lastActivity + config.keepAliveTimeout;
  if (activityExpiry > now) {
    // Active meanwhile: a new observation period starts.
    state.zerocopyStallCheckQueuedBytes = ConnectionState::kNoZerocopyStallCheck;
    _deadlines.upsert(state, fd, activityExpiry);
    return false;
  }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  const auto* asyncState = state.pAsyncState();
  const bool asyncHandlerActive = asyncState != nullptr && asyncState->active;
#else
  static constexpr bool asyncHandlerActive = false;
#endif
  if (config.enableKeepAlive && !state.isTunneling() && !state.tunnelResolving && !state.isSendingFile() &&
      !asyncHandlerActive) {
    // Idle for keepAliveTimeout: close it once its sends complete, as the kernel finishes sending copied data after a
    // close.
    state.requestDrainAndClose();
  }

  // The send queue only shrinks when the peer acknowledges data (or grows when the output progresses): no change during
  // a whole period means that the peer does not read anymore. Its buffers cannot be retained forever: reset the
  // connection (closed gracefully instead by releaseConnection() if its sends completed meanwhile).
  const int64_t queuedBytes = SendQueueBytes(fd);
  if (queuedBytes < 0) [[unlikely]] {
    log::error("ioctl(SIOCOUTQ) failed for fd # {} err={}", fd, LastSystemError());
  }
  const auto queued =
      static_cast<uint32_t>(std::clamp<int64_t>(queuedBytes, 0, int64_t{ConnectionState::kNoZerocopyStallCheck} - 1));
  if (queuedBytes < 0 || queued == state.zerocopyStallCheckQueuedBytes) {
    log::debug("fd # {}: zerocopy sends without progress during keepAliveTimeout, resetting the connection", fd);
    ctx.io.closeConnection(cnxIt);
    ctx.telemetry.counterAdd("aeronet.connections.closed_for_zerocopy_stall");
    return true;
  }
  state.zerocopyStallCheckQueuedBytes = queued;
  _deadlines.upsert(state, fd, now + config.keepAliveTimeout);
  return false;
}
#endif

void ConnectionMaintenance::rebuildKeepAliveDeadlines() {
  ConnectionStorage& connections = _pCtx->connections;
  _deadlines.clear();
  // Not skipped when keep-alive is disabled: WebSocket connections have their own timeouts.
  for (auto cnxIt = connections.begin(); cnxIt != connections.end(); ++cnxIt) {
    if (!IsValid(connections, cnxIt)) {
      continue;
    }
    refreshKeepAliveDeadline(cnxIt);
  }
}

void ConnectionMaintenance::updateBodyReception(ConnectionState& state, bool bodyReady) noexcept {
  if (bodyReady) {
    if (state.waitingForBody) {
      assert(_sweepState.bodyReadConnections > 0U);
      --_sweepState.bodyReadConnections;
      state.waitingForBody = false;
      state.bodyLastActivityMs = ConnectionState::kInactiveRelativeMs;
    }
    return;
  }
  if (_pCtx->config.bodyReadTimeout.count() == 0) {
    return;
  }
  if (!state.waitingForBody) {
    ++_sweepState.bodyReadConnections;
    state.waitingForBody = true;
  }
  state.bodyLastActivityMs = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(state.lastActivity - state.headerStartTp).count());
}

void ConnectionMaintenance::forgetConnectionMaintenance(ConnectionState& state) noexcept {
  _deadlines.remove(state);
  clearRequestDeadline(state);
  _pCtx->io.forgetWritableInterest(state);
  stopHeaderReadTimer(state);
  trackBodyReception(state, true);
#ifdef AERONET_ENABLE_HTTP2
  if (state.protocol == ProtocolType::Http2) {
    assert(_sweepState.http2Connections > 0U);
    --_sweepState.http2Connections;
  }
#endif
}

bool ConnectionMaintenance::needsFullConnectionMaintenanceSweep() const noexcept {
  const ServerContext& ctx = *_pCtx;
  if (ctx.connections.empty()) {
    return false;
  }
  if (ctx.lifecycle.isDraining() || ctx.lifecycle.isStopping()) {
    return true;
  }
  if (ctx.io.nbWritableConnections() != 0U || _sweepState.requestDeadlineConnections != 0U) {
    return true;
  }
  if ((_sweepState.headerReadConnections != 0U && ctx.config.headerReadTimeout.count() > 0) ||
      (_sweepState.bodyReadConnections != 0U && ctx.config.bodyReadTimeout.count() > 0)) {
    return true;
  }
#ifdef AERONET_ENABLE_OPENSSL
  // Only while handshakes are in progress: the established connections need no handshake timeout check.
  if (ctx.tls.nbHandshakesInFlight() != 0U && ctx.config.tls.handshakeTimeout.count() > 0) {
    return true;
  }
#endif
#ifdef AERONET_ENABLE_HTTP2
  if (_sweepState.http2Connections != 0U) {
    return true;
  }
#endif
  return false;
}

void ConnectionMaintenance::sweepIdleConnections() {
  ServerContext& ctx = *_pCtx;
  ConnectionStorage& connections = ctx.connections;
  const auto now = connections.now;
  closeExpiredKeepAliveConnections();

  if (!needsFullConnectionMaintenanceSweep()) {
    ctx.telemetry.gauge("aeronet.connections.cached_count", static_cast<int64_t>(connections.nbCachedConnections()));
    connections.sweepCachedConnections(std::chrono::hours{1});
    return;
  }

  // Cap the number of sendfile / outbound retries per sweep to avoid spending the
  // entire maintenance tick retrying N connections that all return EAGAIN immediately.
  static constexpr int kMaxFlushRetriesPerSweep = 128;
  int flushRetries = 0;

#ifdef AERONET_WINDOWS
  auto cnxIt = connections.begin();
  while (cnxIt != connections.end()) {
#else
  for (auto cnxIt = connections.begin(); cnxIt != connections.end(); ++cnxIt) {
    if (!*cnxIt) {
      continue;
    }
#endif
    ConnectionState& state = connections.connectionState(cnxIt);

    // Retry pending outbound flushes (buffered output, file sends, and the output queued behind them) to handle
    // potential missed EPOLLOUT edges.
    // On Windows, WSAPoll can fail to report writability on loopback sockets, leaving
    // buffered response data (including HTTP/2 DATA frames) stuck in outBuffer indefinitely.
    // Periodic retry here ensures forward progress regardless of missed poll events.
    if ((state.isSendingFile() || state.hasPendingOutput()) && state.waitingWritable &&
        flushRetries < kMaxFlushRetriesPerSweep) {
      ctx.io.flushOutbound(cnxIt);
      ++flushRetries;
    }

#ifdef AERONET_LINUX
    // EPOLLERR reports the zerocopy completions: polled here as well, in case of a missed event.
    ctx.io.pollZerocopyCompletions(state);
#endif
    // For DrainThenClose mode, only close after buffers and file payload are fully drained
    if (state.canCloseConnectionForDrain()) {
      ctx.io.closeConnection(cnxIt);
      ctx.telemetry.counterAdd("aeronet.connections.closed_for_drain");
#ifdef AERONET_WINDOWS
      cnxIt = connections.begin();
#endif
      continue;
    }

    if (closeIfTimedOut(cnxIt, now)) {
#ifdef AERONET_WINDOWS
      cnxIt = connections.begin();
#endif
      continue;
    }

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

  ctx.telemetry.gauge("aeronet.connections.cached_count", static_cast<int64_t>(connections.nbCachedConnections()));

  // Clean up cached connections that have been idle for too long
  connections.sweepCachedConnections(std::chrono::hours{1});
  connections.shrink_to_fit();
}

bool ConnectionMaintenance::closeIfTimedOut(ConnectionIt cnxIt, std::chrono::steady_clock::time_point now) {
  ServerContext& ctx = *_pCtx;
  const HttpServerConfig& config = ctx.config;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  const NativeHandle fd = cnxIt->fd();

  std::string_view timeout;
  std::string_view counter;
  if (config.headerReadTimeout.count() > 0 && state.parsingHeaders &&
      now > state.headerStartTp + config.headerReadTimeout) {
    // Header read timeout: active while headers are being parsed and duration exceeded.
    timeout = "header read";
    counter = "aeronet.connections.closed_for_header_read_timeout";
  } else if (config.bodyReadTimeout.count() > 0 && state.waitingForBody &&
             state.bodyLastActivityMs != ConnectionState::kInactiveRelativeMs &&
             now > state.headerStartTp + std::chrono::milliseconds(state.bodyLastActivityMs) + config.bodyReadTimeout) {
    // Body read timeout: triggered when the handler is waiting for missing body bytes.
    timeout = "body read";
    counter = "aeronet.connections.closed_for_body_read_timeout";
  } else if (state.requestDeadlineMs != ConnectionState::kInactiveRelativeMs &&
             now > state.headerStartTp + std::chrono::milliseconds(state.requestDeadlineMs)) {
    // Per-route request timeout: active when a per-route deadline was set after routing.
    timeout = "per-route request";
    counter = "aeronet.connections.closed_for_request_timeout";
  } else {
#ifdef AERONET_ENABLE_OPENSSL
    // TLS handshake timeout (if enabled). Applies only while handshake pending.
    if (ctx.tls.handshakeTimedOut(state, now)) {
      ctx.tls.failHandshakeOnce(state, fd, kTlsHandshakeFailureReasonHandshakeTimeout);
      ctx.io.closeConnection(cnxIt);
      ctx.telemetry.counterAdd("aeronet.connections.closed_for_handshake_timeout");
      return true;
    }
#endif
    return false;
  }
  log::debug("sweepIdleConnections: fd # {} closed for {} timeout", fd, timeout);
  ctx.http1.emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, {});
  ctx.io.closeConnection(cnxIt);
  ctx.telemetry.counterAdd(counter);
  return true;
}

#ifdef AERONET_ENABLE_WEBSOCKET
void ConnectionMaintenance::refreshWebSocketDeadline(NativeHandle fd, ConnectionState& state) {
  const auto& handler = static_cast<const websocket::WebSocketHandler&>(*state.protocolHandler);
  const auto nextCheck = handler.nextTimeoutCheck(state.lastActivity);
  if (nextCheck == std::chrono::steady_clock::time_point::max()) {
    _deadlines.remove(state);
    return;
  }
  // Like for keep-alive, an armed deadline earlier than the next check is kept: lastActivity only moves forward, so
  // the check re-arms it when it fires. Only the start of a close handshake can bring the next check closer.
  if (!KeepAliveDeadlineQueue::contains(state) || nextCheck < _deadlines.expiresAt(state)) {
    _deadlines.upsert(state, fd, nextCheck);
  }
}

bool ConnectionMaintenance::checkWebSocketTimeouts(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  const NativeHandle fd = cnxIt->fd();
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  auto& handler = static_cast<websocket::WebSocketHandler&>(*state.protocolHandler);
  const auto timeout = handler.checkTimeouts(ctx.connections.now, state.lastActivity);
  if (timeout != websocket::WebSocketHandler::Timeout::None) {
    const bool idle = timeout == websocket::WebSocketHandler::Timeout::Idle;
    log::debug("sweepIdleConnections: fd # {} closed for WebSocket {} timeout", fd, idle ? "idle" : "close");
    ctx.io.closeConnection(cnxIt);
    ctx.telemetry.counterAdd(idle ? "aeronet.connections.closed_for_websocket_idle_timeout"
                                  : "aeronet.connections.closed_for_websocket_close_timeout");
    return true;
  }
  if (handler.hasPendingOutput()) {
    ctx.io.flushOutbound(cnxIt);  // the Ping of an idle connection
  }
  refreshWebSocketDeadline(fd, state);
  return false;
}
#endif

}  // namespace aeronet::internal
