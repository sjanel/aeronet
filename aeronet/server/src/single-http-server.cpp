#include "aeronet/single-http-server.hpp"

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
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/compiler-config.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/internal/connection-io.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/http1-pipeline.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/signal-handler.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "tunnel-manager.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-frame-types.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-metrics.hpp"
#endif

#ifdef AERONET_ENABLE_TEST_HOOKS
#include "aeronet/transport-test-hook.hpp"
#endif

namespace aeronet {

// ============================================================================
// Event loop
// ============================================================================

void SingleHttpServer::eventLoop() {
  // Apply any pending config updates posted from other threads.
  applyPendingUpdates();

  if (_ctx.tunnels != nullptr && _ctx.tunnels->hasResolvedTargets()) {
    _ctx.tunnels->completeResolvedTunnels();
  }

  // Poll for events. The connections deferred by ConnectionIo::deferInput() are served at the end of this iteration:
  // the poll then only collects the events already ready, without waiting.
  const auto events = _ctx.eventLoop.poll(!_ctx.io.hasDeferredInputs());

  // Update cached now time
  const auto now = std::chrono::steady_clock::now();
  _ctx.connections.now = now;
  _ctx.dateHeader.refreshIfNeeded(now);

  // Publish the loop heartbeat (see internal::Lifecycle::loopHeartbeat): a single relaxed store of the 'now' we just
  // read. A dedicated probe listener (MultiHttpServer, see BuiltinProbesConfig::dedicatedPort) reads it to tell a loop
  // wedged inside a request handler from an idle or progressing one - without any bookkeeping on the handler hot path.
  _ctx.lifecycle.loopHeartbeat(now);

  bool maintenanceTick = false;

  if (events.data() == nullptr) [[unlikely]] {
    _ctx.telemetry.counterAdd("aeronet.events.errors", 1);
    _ctx.lifecycle.exchangeStopping();
  } else if (!events.empty()) {
    // Read before the batch: the wakeup below may close the listener, whose event already polled in this batch must
    // then not be taken for the event of a connection.
    const NativeHandle listenFd = _listenSocket.fd();
    for (auto event : events) {
      const auto fd = event.fd;
      if (fd == listenFd) {
        // Always attempt to accept new connections when the listener is signaled (unless closed since).
        // The lifecycle controls higher-level acceptance semantics; accepting
        // here is safe and allows probes to connect during drain.
        if (_listenSocket) {
          acceptNewConnections();
        }
      } else if (fd == _ctx.lifecycle.wakeupFd.fd()) {
        _ctx.lifecycle.wakeupFd.read();
        // When stop() is called from another thread it sets the state to Stopping
        // and sends a wakeup.  Close the listener immediately so no new connections
        // are accepted while we wait for the maintenance tick to clean up.
        if (_ctx.lifecycle.isStopping()) {
          closeListener();
          maintenanceTick = true;
        }
      } else if (fd == _maintenanceTimer.fd()) {
        _maintenanceTimer.drain();
        maintenanceTick = true;
      } else {
        handleConnectionEvent(event, now);
      }
    }
    // Request handlers run from the read path and may have blocked for longer than keepAliveTimeout.
    restartKeepAliveIdleWindows(events, now);
    _ctx.telemetry.counterAdd("aeronet.events.processed", static_cast<uint64_t>(events.size()));
  } else {
    // timeout / error::kInterrupted (treated as timeout). Retry pending writes to handle edge-triggered epoll timing
    // issues. With EPOLLET, if a socket becomes writable after sendfile() returns EAGAIN but before
    // epoll_ctl(EPOLL_CTL_MOD), we miss the edge. Periodic retries ensure we eventually resume.
    maintenanceTick = true;
  }

  if (_ctx.io.hasDeferredInputs()) {
    resumeDeferredInputs();
  }

  // Under high load epoll_wait may return immediately and never hit the timeout path.
  // We still need periodic maintenance for timeouts and edge-triggered sendfile progress.
  // On Windows and macOS the TimerFd does not fire real events (armPeriodic is a no-op);
  // maintenance runs only when poll returns empty (timeout). Under sustained event activity
  // the poll may never timeout, starving maintenance. Fall back to a time-based check.
#ifndef AERONET_LINUX
  if (!maintenanceTick && now - _lastMaintenanceTp >= _ctx.config.pollInterval) {
    maintenanceTick = true;
  }
#endif
  if (maintenanceTick) {
    runMaintenance(now);
  }
}

AERONET_ALWAYS_INLINE void SingleHttpServer::handleConnectionEvent(EventLoop::EventFd event,
                                                                   std::chrono::steady_clock::time_point now) {
  internal::ConnectionStorage& connections = _ctx.connections;
  const auto fd = event.fd;
  const auto bmp = event.eventBmp;
  const auto cnxIt = connections.iterator(fd);
  if (!IsValid(connections, cnxIt)) [[unlikely]] {
    log::warn("fd # {} not found (stale epoll event or race)", fd);
    // Remove stale entry from the event-loop poller so it doesn't keep firing.
    // Safe to call here because we are inside the event-loop thread.
    _ctx.eventLoop.del(fd);
    return;
  }

  ConnectionState& eventState = connections.connectionState(cnxIt);
  eventState.lastActivity = now;
  _ctx.maintenance.refreshKeepAliveDeadline(cnxIt);

  CloseStatus closeStatus = CloseStatus::Keep;
  // EPOLLERR/EPOLLHUP/EPOLLRDHUP can be delivered without EPOLLIN.
  // Treat them as a read trigger so we promptly observe EOF/errors and close.
  EventBmp readEvents = bmp & (EventIn | EventErr | EventHup | EventRdHup);
#ifdef AERONET_LINUX
  // EPOLLERR also reports the zerocopy completion notifications queued in the socket error queue: read them,
  // releasing the buffers of the completed sends. A socket error (reset, timeout) comes with EPOLLHUP: an
  // EPOLLERR explained by notifications calls for no read.
  bool zerocopyCompletions = false;
  if ((bmp & EventErr) != 0 && _ctx.io.pollZerocopyCompletions(eventState).nbSends != 0) {
    readEvents &= ~EventErr;
    zerocopyCompletions = true;
  }
#endif
  bool readTrigger = readEvents != 0;
  if ((bmp & EventOut) != 0) {
#ifdef AERONET_ENABLE_OPENSSL
    // A TLS handshake waiting for the socket to be writable (its flight did not fit in the socket send buffer)
    // is driven by the read path: the writable event alone would only flush the (empty) output.
    readTrigger = readTrigger || (eventState.transport.isTls() && !eventState.transport.handshakeDone());
#endif
    closeStatus = handleWritableClient(cnxIt);
  }
  if (readTrigger) {
    closeStatus =
        std::max(handleReadableClient(cnxIt, (readEvents & (EventErr | EventHup | EventRdHup)) == 0), closeStatus);
  }
#ifdef AERONET_LINUX
  else if (zerocopyCompletions && (bmp & EventOut) == 0 && eventState.canCloseConnectionForDrain()) {
    // A drain close was waiting for these completions.
    closeStatus = CloseStatus::Close;
  }
#endif
  if (closeStatus == CloseStatus::Close) {
    // A handler (e.g. the half-close of a tunnel peer) may have already recycled
    // this connection via a nested closeConnection call. Guard against that.
    const auto finalIt = connections.iterator(fd);
    if (IsValid(connections, finalIt)) {
      _ctx.io.closeConnection(finalIt);
    }
  }
}

void SingleHttpServer::resumeDeferredInputs() {
  // Re-process connections deferred by the per-event fairness cap, or whose input was left by the output backpressure.
  // Edge-triggered polling (EPOLLET / EV_CLEAR) only fires on state transitions;
  // a connection that still had TCP data after hitting the cap won't generate a new
  // read event, so we must re-read it here before waiting for events again.
  //
  // Taken out of ConnectionIo so that resumeDeferredInput can safely defer inputs again without invalidating our
  // iteration and without losing them to a blanket clear().
  for (const NativeHandle pendingFd : _ctx.io.takeDeferredInputs()) {
    const auto pendingIt = _ctx.connections.iterator(pendingFd);
    if (!IsValid(_ctx.connections, pendingIt)) {
      continue;
    }
    completeOutOfBatchRead(pendingFd, resumeDeferredInput(pendingIt));
  }
}

void SingleHttpServer::runMaintenance(std::chrono::steady_clock::time_point now) {
#ifndef AERONET_LINUX
  _lastMaintenanceTp = now;
#endif
  const auto nbActiveConnections = _ctx.connections.size();

  _ctx.telemetry.gauge("aeronet.connections.active_count", static_cast<int64_t>(nbActiveConnections));
  _ctx.telemetry.gauge("aeronet.events.capacity_current_count", static_cast<int64_t>(_ctx.eventLoop.capacity()));

  _ctx.maintenance.sweepIdleConnections();

  if (_ctx.tunnels != nullptr) {
    _ctx.tunnels->expirePendingTunnels(now);
  }

#ifdef AERONET_LINUX
  _ctx.io.releaseResetZerocopyBuffers(now);
#endif

  if (_ctx.accessLog) {
    _ctx.accessLog.flush();
  }

  internal::Lifecycle& lifecycle = _ctx.lifecycle;
  if (lifecycle.isStopping() || (lifecycle.isDraining() && nbActiveConnections == 0)) {
    closeListenerAndConnections();
    lifecycle.reset();
    if (!isInMultiHttpServer()) {
      log::info("Server stopped");
    }
  } else if (lifecycle.isDraining()) {
    if (lifecycle.drainDeadlineReached(now)) {
      log::warn("Drain deadline reached with {} active connection(s); forcing close", nbActiveConnections);
      closeListenerAndConnections();
      lifecycle.reset();
      log::info("Server drained after deadline");
    }
  } else if (SignalHandler::IsStopRequested()) {
    beginDrain(SignalHandler::GetMaxDrainPeriod());
  }

  // After the closes above, whose counters are then exported without waiting for the next tick.
  _ctx.telemetry.flush();

  // Also shrink per-thread scratch buffers used during decompression / header parsing, and the input buffer lent to the
  // reads.
  _ctx.sharedBuffers.shrink_to_fit();
  _ctx.io.shrinkLentInBuffer();
}

void SingleHttpServer::updateMaintenanceTimer() {
  // Periodic maintenance timer: drives idle sweeps / housekeeping without relying on epoll_wait timeouts.
  using namespace std::chrono;

  const HttpServerConfig& config = _ctx.config;
  milliseconds minTimeout = milliseconds::max();
  const auto consider = [&](milliseconds dur) {
    if (dur.count() > 0) {
      minTimeout = std::min(minTimeout, dur);
    }
  };

  if (config.enableKeepAlive) {
    consider(config.keepAliveTimeout);
  }
  consider(config.headerReadTimeout);
  consider(config.bodyReadTimeout);
  consider(config.pollInterval);

#ifdef AERONET_ENABLE_OPENSSL
  if (config.tls.enabled) {
    consider(config.tls.handshakeTimeout);
  }
#endif

  assert(minTimeout != milliseconds::max());

  _maintenanceTimer.armPeriodic(minTimeout);
}

void SingleHttpServer::closeListener() noexcept {
  if (_listenSocket) {
#ifndef AERONET_WINDOWS
    // On POSIX, epoll_ctl(DEL) / kevent(EV_DELETE) are kernel-level thread-safe,
    // so removing the fd from the event loop while the poll thread is blocked is fine.
    _ctx.eventLoop.del(_listenSocket.fd());
#endif
    // On Windows, EventLoop::del() mutates the user-space WSAPoll array.
    // Calling it from a different thread while WSAPoll() is blocked is a data race.
    // Just close the socket; the next WSAPoll() iteration will report POLLNVAL for
    // the stale entry, which the event loop handles gracefully (fd won't match
    // listen/wakeup/timer and the connection lookup returns not-found — logged and skipped).
    _listenSocket.close();
    // Trigger wakeup to break any blocking poll quickly.
    _ctx.lifecycle.wakeupFd.send();
  }
}

void SingleHttpServer::closeListenerAndConnections() {
  closeListener();
  _ctx.io.closeAllConnections();
}

bool SingleHttpServer::isServerInternalFd(NativeHandle fd) const noexcept {
  return fd == _listenSocket.fd() || fd == _ctx.lifecycle.wakeupFd.fd() || fd == _maintenanceTimer.fd();
}

void SingleHttpServer::restartKeepAliveIdleWindows(std::span<const EventLoop::EventFd> events,
                                                   std::chrono::steady_clock::time_point batchStart) {
  const HttpServerConfig& config = _ctx.config;
  if (!config.enableKeepAlive) {
    return;
  }
  const auto workEnd = std::chrono::steady_clock::now();
  if (batchStart + config.keepAliveTimeout > workEnd) {
    return;  // the whole batch was served well within the idle window: no armed deadline can be stale
  }
  for (const auto event : events) {
    if (!isServerInternalFd(event.fd)) {
      _ctx.maintenance.restartKeepAliveIdleWindow(event.fd, workEnd);
    }
  }
}

ServerStats SingleHttpServer::stats() const {
  const internal::ServerStatsInternal& stats = _ctx.stats;
  ServerStats statsOut;
  statsOut.totalBytesQueued = stats.totalBytesQueued.load();
  statsOut.totalBytesWrittenImmediate = stats.totalBytesWrittenImmediate.load();
  statsOut.totalBytesWrittenFlush = stats.totalBytesWrittenFlush.load();
  statsOut.deferredWriteEvents = stats.deferredWriteEvents.load();
  statsOut.flushCycles = stats.flushCycles.load();
  statsOut.epollModFailures = stats.epollModFailures.load();
  statsOut.maxConnectionOutboundBuffer = stats.maxConnectionOutboundBuffer.load();
  statsOut.totalRequestsServed = stats.totalRequestsServed.load();
  statsOut.zerocopySends = stats.zerocopySends.load();
  statsOut.zerocopyBytes = stats.zerocopyBytes.load();
  statsOut.zerocopyCopiedSends = stats.zerocopyCopiedSends.load();
  statsOut.zerocopyNoBufferSpaceFallbacks = stats.zerocopyNoBufferSpaceFallbacks.load();
  statsOut.zerocopyRetainLimitFallbacks = stats.zerocopyRetainLimitFallbacks.load();
  statsOut.zerocopyResetConnections = stats.zerocopyResetConnections.load();
#ifdef AERONET_ENABLE_OPENSSL
  const TlsMetricsInternal& tlsMetrics = _ctx.tls.metrics();
  statsOut.tlsHandshakesSucceeded = tlsMetrics.handshakesSucceeded.load();
  statsOut.tlsHandshakesFull = tlsMetrics.handshakesFull.load();
  statsOut.tlsHandshakesResumed = tlsMetrics.handshakesResumed.load();
  statsOut.tlsHandshakesFailed = tlsMetrics.handshakesFailed.load();
  statsOut.tlsHandshakesRejectedConcurrency = tlsMetrics.handshakesRejectedConcurrency.load();
  statsOut.tlsHandshakesRejectedRateLimit = tlsMetrics.handshakesRejectedRateLimit.load();
  statsOut.tlsClientCertPresent = tlsMetrics.clientCertPresent.load();
  statsOut.tlsAlpnStrictMismatches = tlsMetrics.alpnStrictMismatches.load();
  // The maps are updated by the event loop on TLS handshakes.
  std::scoped_lock lock(tlsMetrics.mapsMutex.mutex);
  statsOut.tlsAlpnDistribution.reserve(tlsMetrics.alpnDistribution.size());
  for (const auto& [key, value] : tlsMetrics.alpnDistribution) {
    statsOut.tlsAlpnDistribution.emplace_back(key, value);
  }
  statsOut.tlsHandshakeFailureReasons.reserve(tlsMetrics.handshakeFailureReasons.size());
  for (const auto& [key, value] : tlsMetrics.handshakeFailureReasons) {
    statsOut.tlsHandshakeFailureReasons.emplace_back(key, value);
  }
  statsOut.tlsVersionCounts.reserve(tlsMetrics.versionCounts.size());
  for (const auto& [key, value] : tlsMetrics.versionCounts) {
    statsOut.tlsVersionCounts.emplace_back(key, value);
  }
  statsOut.tlsCipherCounts.reserve(tlsMetrics.cipherCounts.size());
  for (const auto& [key, value] : tlsMetrics.cipherCounts) {
    statsOut.tlsCipherCounts.emplace_back(key, value);
  }
  statsOut.tlsHandshakeDurationCount = tlsMetrics.handshakeDurationCount.load();
  statsOut.tlsHandshakeDurationTotalNs = tlsMetrics.handshakeDurationTotalNs.load();
  statsOut.tlsHandshakeDurationMaxNs = tlsMetrics.handshakeDurationMaxNs.load();
  statsOut.ktlsSendEnabledConnections = tlsMetrics.ktlsSendEnabledConnections.load();
  statsOut.ktlsSendEnableFallbacks = tlsMetrics.ktlsSendEnableFallbacks.load();
  statsOut.ktlsSendForcedShutdowns = tlsMetrics.ktlsSendForcedShutdowns.load();
  statsOut.ktlsSendBytes = tlsMetrics.ktlsSendBytes.load();
#endif
  return statsOut;
}

// ============================================================================
// Connections
// ============================================================================

void SingleHttpServer::acceptNewConnections() {
  internal::ConnectionStorage& connections = _ctx.connections;
  const HttpServerConfig& config = _ctx.config;
  // Cap the number of connections accepted per event-loop iteration to avoid starving existing connections when a burst
  // of new connections arrives (e.g. wrk opening 1000b connections simultaneously).  Remaining connections stay in the
  // kernel backlog and will be accepted on the next EPOLLIN on the listen socket.
  const auto maxAcceptBatch = config.maxAcceptBatchSize;
  assert(maxAcceptBatch > 0);
  for (decltype(config.maxAcceptBatchSize) accepted = 0; accepted < maxAcceptBatch;) {
    sockaddr_storage peerAddress;
    Connection cnx(_listenSocket, peerAddress);
    if (!cnx) {
      // no more waiting connections
      break;
    }
    const auto cnxFd = cnx.fd();
    bool tcpNoDelayActive = false;
    if (config.tcpNoDelay == TcpNoDelayMode::Enabled) {
      if (SetTcpNoDelay(cnxFd)) [[likely]] {
        tcpNoDelayActive = true;
      } else {
        const auto err = LastSystemError();
        log::error("setsockopt(TCP_NODELAY) failed for fd # {} err={}", cnxFd, err);
        _ctx.telemetry.counterAdd("aeronet.connections.errors.tcp_nodelay_failed", 1UL);
      }
    }
    if (!_ctx.eventLoop.add(EventLoop::EventFd{cnxFd, EventIn | EventRdHup | EventEt})) [[unlikely]] {
      _ctx.telemetry.counterAdd("aeronet.connections.errors.add_event_failed", 1UL);
      continue;
    }

    const ConnectionIt cnxIt = connections.emplace(std::move(cnx));

    _ctx.telemetry.counterAdd("aeronet.connections.accepted", 1UL);

    ConnectionState& state = connections.connectionState(cnxIt);

    state.initializeStateNewConnection(config, peerAddress, _ctx.compressionState);

    // TCP_NODELAY disables Nagle — mark corkable so response writes use TCP_CORK to coalesce.
    state.corkable = tcpNoDelayActive;

#ifdef AERONET_ENABLE_OPENSSL
    if (_ctx.tls.hasContext()) {
      if (!_ctx.tls.startHandshake(state, cnxFd)) {
        _ctx.io.closeConnection(cnxIt);
        continue;
      }
    } else {
      state.transport = Transport(cnxFd);
    }
#else
    state.transport = Transport(cnxFd);
#endif

#ifdef AERONET_ENABLE_TEST_HOOKS
    // Test hook: allow tests to wrap/decorate the transport for fault injection.
    state.transport = test::ApplyTransportDecorator(std::move(state.transport));
#endif

#ifdef AERONET_LINUX
    // MSG_ZEROCOPY needs a plain socket: accepted ones inherit SO_ZEROCOPY from the listener (see initListener()).
    state.zerocopyEnabled = state.zerocopyEnabled && _ctx.io.zerocopySupported() && state.transport.isPlain();
#endif

    _ctx.maintenance.refreshKeepAliveDeadline(cnxIt);

    // The header read timer of its first request starts now: a client that does not send it is answered 408 once
    // headerReadTimeout elapsed. The timers of the next ones start with their first bytes (see handleReadableClient()).
    _ctx.maintenance.startHeaderReadTimer(state);

    // Serve the input that came with the connection setup (TLS ClientHello, request, h2c preface...) without waiting
    // for its first readable event.
    completeOutOfBatchRead(cnxFd, handleReadableClient(cnxIt, true));

    ++accepted;
  }
}

bool SingleHttpServer::finalizeTlsHandshakeIfReady([[maybe_unused]] NativeHandle fd, ConnectionState& state) {
  if (state.tlsEstablished || !state.transport.handshakeDone()) {
    return false;
  }
#ifdef AERONET_ENABLE_OPENSSL
  _ctx.tls.finalizeHandshake(fd, state);
#ifdef AERONET_ENABLE_HTTP2
  if (_ctx.config.http2.enable && state.tlsInfo.selectedAlpn() == http2::kAlpnH2) {
    setupHttp2Connection(fd, _ctx.config.tcpNoDelay, state);
  }
#endif
#endif
  state.tlsEstablished = true;
  return state.isAnyCloseRequested();
}

SingleHttpServer::CloseStatus SingleHttpServer::handleWritableClient(ConnectionIt cnxIt) {
  ConnectionState& state = _ctx.connections.connectionState(cnxIt);

  const auto fd = cnxIt->fd();
  if (state.isTunneling() && _ctx.tunnels->handleWritable(cnxIt) == CloseStatus::Close) {
    return CloseStatus::Close;
  }
  _ctx.io.flushOutbound(cnxIt);

  // Once EPOLLOUT drained the output, resume the input left by its backpressure directly: edge-triggered polling may
  // not deliver another read event.
  processBufferedInput(fd);
  ConnectionState* pState = _ctx.connections.pConnectionState(fd);
  assert(pState != nullptr);  // the input processing only requests closes
  // The records that the TLS transport read ahead while the input processing was stopped are not reported by the
  // kernel either.
  if (!pState->hasPendingOutput() && !pState->isAnyCloseRequested() && pState->transport.hasPendingReadData()) {
    _ctx.io.deferInput(fd, *pState);
  }
  return pState->canCloseConnectionForDrain() ? CloseStatus::Close : CloseStatus::Keep;
}

void SingleHttpServer::processBufferedInput(NativeHandle fd) {
  // Output backpressure can leave complete input in the user-space input buffer: HTTP/2 frames above the output
  // high-water mark, or HTTP/1 requests pipelined behind a response that waited for the socket (not processed after a
  // close request). So can the fairness budget, once the responses to the pipelined HTTP/1 requests reached it.
  ConnectionState* pState = _ctx.connections.pConnectionState(fd);
  if (pState == nullptr || pState->hasPendingOutput() || pState->inBuffer.empty()) {
    return;
  }
  if (pState->protocolHandler) {
    (void)processSpecialProtocolHandler(_ctx.connections.iterator(fd));
  } else if (!pState->isSendingFile() && !pState->isTunneling() && !pState->isAnyCloseRequested()) {
    (void)processHttp1Input(_ctx.connections.iterator(fd));
  }
}

SingleHttpServer::CloseStatus SingleHttpServer::resumeDeferredInput(ConnectionIt cnxIt) {
  const auto fd = cnxIt->fd();
  ConnectionState& deferredState = _ctx.connections.connectionState(cnxIt);
  deferredState.inputDeferred = false;
  if (deferredState.isLingering()) {
    return _ctx.io.discardLingeringInput(cnxIt);
  }
  processBufferedInput(fd);
  ConnectionState& state = _ctx.connections.connectionState(_ctx.connections.iterator(fd));
  if (state.inputDeferred || state.isAnyCloseRequested()) {
    // Its responses used the fairness budget again, or it closes: nothing more to read now.
    return state.canCloseConnectionForDrain() ? CloseStatus::Close : CloseStatus::Keep;
  }
  return handleReadableClient(_ctx.connections.iterator(fd), false);
}

void SingleHttpServer::completeOutOfBatchRead(NativeHandle fd, CloseStatus closeStatus) {
  if (closeStatus == CloseStatus::Close) {
    if (const auto cnxIt = _ctx.connections.iterator(fd); IsValid(_ctx.connections, cnxIt)) {
      _ctx.io.closeConnection(cnxIt);
    }
  } else {
    // Not covered by restartKeepAliveIdleWindows(): the requests read here (such as the ones that a client sends with
    // its connection setup, like an h2c client with prior knowledge) may have run handlers outliving keepAliveTimeout.
    _ctx.maintenance.restartKeepAliveIdleWindow(fd);
  }
}

SingleHttpServer::CloseStatus SingleHttpServer::handleReadableClient(ConnectionIt cnxIt, bool stopOnShortRead) {
  internal::ConnectionStorage& connections = _ctx.connections;
  const HttpServerConfig& config = _ctx.config;
  ConnectionState* pCnx = connections.pConnectionState(cnxIt);
  assert(pCnx != nullptr);

  if (pCnx->isLingering()) [[unlikely]] {
    return _ctx.io.discardLingeringInput(cnxIt);
  }

  // Pending output can legitimately be non-empty when we get EPOLLIN.
  // This happens with partial writes and very commonly with TLS (SSL_read/handshake progress
  // can generate outbound records that must be written before further progress).
  // Opportunistically flush here; if still blocked on write, yield and wait for EPOLLOUT.
  if (pCnx->hasPendingOutput()) {
    _ctx.io.flushOutbound(cnxIt);
    if (pCnx->hasPendingOutput()) {
      if (!pCnx->waitingWritable) {
        _ctx.io.enableWritableInterest(cnxIt);
      }
      return CloseStatus::Keep;
    }
  }

  // A tunnel endpoint relays raw bytes to its peer.
  if (pCnx->isTunneling() || pCnx->tunnelResolving) {
    return _ctx.tunnels->handleReadable(cnxIt);
  }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  if (pCnx->protocolHandler == nullptr && internal::Http1Pipeline::PauseReadingDuringAsyncHandler(*pCnx)) {
    return pCnx->canCloseConnectionForDrain() ? CloseStatus::Close : CloseStatus::Keep;
  }
#endif

  std::size_t bytesReadThisEvent = 0;
  // The bytes written while serving the input count toward the fairness budget too: a small request can have a large
  // response, so a client pipelining such requests (an HTTP/2 client keeping several streams busy, say) would otherwise
  // keep the event loop on its connection, while the other ones wait, until it has sent a whole budget of requests.
  const uint64_t bytesWrittenAtEventStart = _ctx.totalBytesWritten();
  const auto fd = cnxIt->fd();

  // Without pending input, the connection reads into the input buffer of the event loop rather than into its own:
  // HTTP/2 or WebSocket messages of tens of kilobytes on many connections would otherwise be received in cold memory,
  // and each idle keep-alive connection would hold an input buffer. It is released below if the input was entirely
  // served, the connection keeping it otherwise.
  _ctx.io.lendInBuffer(*pCnx);

  while (true) {
    // Re-set the pointer on each loop iteration in case of connection state reallocations.
    cnxIt = connections.iterator(fd);
    pCnx = connections.pConnectionState(fd);

    const std::size_t chunkSize = config.computeReadChunkSize(bytesReadThisEvent, pCnx->readSizeShift);
    assert(chunkSize > 0);
    const auto [count, want] = pCnx->transportRead(config, chunkSize);
    // If the TLS handshake completed during this read, finalize it before the error / EOF handling below: its result
    // (negotiated ALPN, cipher, version, client certificate, metrics) is recorded even if the same read failed.
    if (finalizeTlsHandshakeIfReady(fd, *pCnx)) {
      return CloseStatus::Close;
    }
    if (count != 0) {
      _ctx.maintenance.trackReceivedBytes(*pCnx, connections.now);
    }
    if (want == TransportHint::Error) [[unlikely]] {
      log::debug("Closing fd # {} on transport read error err={}", fd, LastSystemError());
#ifdef AERONET_ENABLE_OPENSSL
      _ctx.tls.reportInterruptedHandshake(*pCnx, fd, want);
#endif
      // No input to wait for in a lingering close.
      pCnx->eofReceived = true;
      return CloseStatus::Close;
    }
    if (want != TransportHint::None) {
      // Non-fatal: transport needs the socket to be readable or writable before proceeding.
      if (want == TransportHint::WriteReady && !pCnx->waitingWritable && !_ctx.io.enableWritableInterest(cnxIt)) {
        return CloseStatus::Close;
      }
      break;
    }
    if (count == 0) {
#ifdef AERONET_ENABLE_OPENSSL
      _ctx.tls.reportInterruptedHandshake(*pCnx, fd);
#endif
      pCnx->eofReceived = true;
      // An orderly EOF only tells that the peer stopped sending: while the kernel still sends with zerocopy, close once
      // these sends completed instead of aborting them (see ConnectionIo::closeConnection()).
      if (pCnx->hasZerocopySendsInFlight()) {
        pCnx->requestDrainAndClose();
        return CloseStatus::Keep;
      }
      return CloseStatus::Close;
    }

    bytesReadThisEvent += static_cast<std::size_t>(count);
    _ctx.telemetry.counterAdd("aeronet.bytes.read", static_cast<uint64_t>(count));

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
    cnxIt = connections.iterator(fd);
    pCnx = connections.pConnectionState(fd);
    if (pCnx->isTunneling()) {
      return _ctx.tunnels->relayInput(cnxIt);
    }

    // A drained read left nothing to read, and later data raises a new event: no need to defer the connection.
    if (!drained &&
        config.fairnessBudgetExhausted(bytesReadThisEvent +
                                       static_cast<std::size_t>(_ctx.totalBytesWritten() - bytesWrittenAtEventStart))) {
      // Edge-triggered polling (EPOLLET / EV_CLEAR): data may remain in the TCP buffer
      // after the fairness cap. No new read event fires on a non-empty→non-empty transition,
      // so defer this fd to read it again after the other ready connections have been served.
      _ctx.io.deferInput(fd, *pCnx);
      break;
    }

    // The head of the current HTTP/1.1 request may still be incomplete after this read: bound its size and its
    // reception time.
    if (pCnx->parsingHeaders && _ctx.http1.rejectIncompleteHead(cnxIt)) {
      return CloseStatus::Close;
    }

    if (drained) {
      break;
    }
  }
  // The input processing may have inserted connections.
  cnxIt = connections.iterator(fd);
  pCnx = connections.pConnectionState(fd);
  _ctx.io.releaseServedInBuffer(*pCnx);
  // Try to flush again after reading new data, in case TLS needed the read to proceed with write
  if (pCnx->hasPendingOutput()) {
    _ctx.io.flushOutbound(cnxIt);
  }
  return pCnx->canCloseConnectionForDrain() ? CloseStatus::Close : CloseStatus::Keep;
}

// ============================================================================
// Protocol dispatch
// ============================================================================

bool SingleHttpServer::processConnectionInput(ConnectionIt cnxIt) {
  ConnectionState& state = _ctx.connections.connectionState(cnxIt);

  // If we have a protocol handler installed (e.g., WebSocket, HTTP/2), use it
  if (state.protocolHandler) {
    return processProtocolHandlerInput(cnxIt);
  }

  // Check for h2c prior knowledge: client sending HTTP/2 connection preface directly
  // The preface is "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" (24 bytes)
  // h2c prior knowledge only applies to non-TLS (plaintext) connections
#ifdef AERONET_ENABLE_HTTP2
  const HttpServerConfig& config = _ctx.config;
  if (config.http2.enable && config.http2.enableH2c && !config.tls.enabled) {
    std::string_view bufView(state.inBuffer);
    // Check if buffer starts with "PRI " (first 4 chars of HTTP/2 preface)
    if (bufView.starts_with("PRI ")) {
      // Wait for full preface if we don't have enough data
      if (bufView.size() < http2::kConnectionPreface.size()) {
        return false;  // Need more data
      }
      // Verify full preface
      if (bufView.starts_with(http2::kConnectionPreface)) {
        // Switch to the HTTP/2 protocol handler, which waits for the client preface before sending SETTINGS.
        installHttp2Handler(cnxIt->fd(), state, false);
        return processSpecialProtocolHandler(cnxIt);
      }
      log::error("Invalid HTTP/2 preface, falling back to HTTP/1.1");
      // Invalid preface - continue with HTTP/1.1 (will likely fail with 400)
    }
  }
#endif

  // Default to HTTP/1.1 request processing
  return processHttp1Input(cnxIt);
}

bool SingleHttpServer::processProtocolHandlerInput(ConnectionIt cnxIt) {
#ifdef AERONET_ENABLE_WEBSOCKET
  if (_ctx.connections.connectionState(cnxIt).protocol == ProtocolType::WebSocket) {
    const NativeHandle fd = cnxIt->fd();
    const bool closeRequested = processSpecialProtocolHandler(cnxIt);
    // A callback may have started the close handshake, whose timeout can come before the armed deadline.
    _ctx.maintenance.refreshWebSocketDeadline(fd, *_ctx.connections.pConnectionState(fd));
    return closeRequested;
  }
#endif
  return processSpecialProtocolHandler(cnxIt);
}

bool SingleHttpServer::processHttp1Input(ConnectionIt cnxIt) {
  const NativeHandle fd = cnxIt->fd();
  if (_ctx.http1.processRequests(cnxIt)) {
    return true;
  }
  // The bytes following an upgrade request belong to the new protocol: no read event would report them. An upgraded
  // connection never goes back to HTTP/1.
  cnxIt = _ctx.connections.iterator(fd);
  const ConnectionState& state = _ctx.connections.connectionState(cnxIt);
  if (state.protocolHandler != nullptr && !state.inBuffer.empty() && !state.isAnyCloseRequested()) {
    return processProtocolHandlerInput(cnxIt);
  }
  return false;
}

bool SingleHttpServer::processSpecialProtocolHandler(ConnectionIt cnxIt) {
  internal::ConnectionStorage& connections = _ctx.connections;
  // Save the client fd before entering the loop. processInput() may set up a tunnel through the tunnel bridge
  // (HTTP/2 CONNECT), whose insertion may invalidate cnxIt by growing the POSIX fd vector or rehashing the Windows
  // map. Re-find cnxIt by fd after each processInput() call, matching the HTTP/1.1 CONNECT path.
  const NativeHandle clientFd = cnxIt->fd();

  ConnectionState* pState = connections.pConnectionState(clientFd);
  assert(pState != nullptr);
  auto& handler = *pState->protocolHandler;

  // Process input in a loop until no more bytes can be consumed.
  // This is important for HTTP/2 where the client may send multiple frames
  // (e.g., connection preface + SETTINGS) in a single TCP packet.
  //
  // Batch input processing, then gather protocol-owned frame fragments directly.
  bool hasAccumulatedOutput = handler.hasPendingOutput();

  while (!pState->inBuffer.empty()) {
    // Convert input buffer to span of bytes
    std::span<const std::byte> inputData(reinterpret_cast<const std::byte*>(pState->inBuffer.data()),
                                         pState->inBuffer.size());

    // Process input through the protocol handler.
    const auto result = handler.processInput(inputData);

    // processInput may insert new elements in connections.
    cnxIt = connections.iterator(clientFd);
    pState = connections.pConnectionState(clientFd);

    if (result.outputRefersToInput) {
      // The output sends data from the input buffer (the payload of a WebSocket echo, say): write it before the input
      // is erased. The handler copies the part left unsent, which waits for the writable event.
      _ctx.io.flushOutbound(cnxIt);
      handler.releaseInput();
      hasAccumulatedOutput = false;
    } else if (handler.hasPendingOutput()) {
      // Remember that the handler owns output to gather once this input batch finishes.
      hasAccumulatedOutput = true;
    }

    // Consume processed bytes from input buffer
    pState->inBuffer.erase_front(result.bytesConsumed);

    // Handle result
    switch (result.action) {
      case ProtocolProcessResult::Action::Continue:
        [[fallthrough]];
      case ProtocolProcessResult::Action::ResponseReady:
        // ResponseReady was already handled above via getPendingOutput
        // If no bytes were consumed, flush before deciding whether the socket can accept more input.
        if (result.bytesConsumed == 0) {
          if (hasAccumulatedOutput) {
            _ctx.io.flushOutbound(cnxIt);
          }
          return StopReadingIfOutputBlocked(*pState);
        }
        break;

      case ProtocolProcessResult::Action::Upgrade:
        // Should not happen for WebSocket/HTTP2 handler
        log::warn("Unexpected upgrade action from protocol handler");
        break;

      case ProtocolProcessResult::Action::Close:
        // Protocol wants to close gracefully - flush GOAWAY etc. before closing
        if (hasAccumulatedOutput) {
          _ctx.io.flushOutbound(cnxIt);
        }
        pState->requestDrainAndClose();
        return true;

      case ProtocolProcessResult::Action::CloseImmediate:
        // Protocol error - flush any error frames then close immediately
        if (hasAccumulatedOutput) {
          _ctx.io.flushOutbound(cnxIt);
        }
        log::debug("Protocol handler reported error");
        pState->requestDrainAndClose();
        return true;
    }
  }

  // Batch-flush all accumulated output frames in a single transport write.
  if (hasAccumulatedOutput) {
    _ctx.io.flushOutbound(cnxIt);
  }

  return StopReadingIfOutputBlocked(*pState);
}

bool SingleHttpServer::StopReadingIfOutputBlocked(ConnectionState& state) {
  // A blocked output transport is also a signal to stop this readable-event loop. This keeps additional
  // HTTP/2 frames in the socket while the peer is not draining responses. The kernel reports them again once the output
  // drained, but not the frames already in the input buffer or in the records read ahead by the TLS transport.
  const bool outputBlocked = state.hasPendingOutput();
  state.inputBlockedByOutput = outputBlocked && (!state.inBuffer.empty() || state.transport.hasPendingReadData());
  return state.isAnyCloseRequested() || outputBlocked;
}

}  // namespace aeronet
