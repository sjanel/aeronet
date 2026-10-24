#include "tunnel-manager.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/connect-target.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/connection-io.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/vector.hpp"
#include "http-error-build.hpp"
#include "tunnel-resolver.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-error-code-name.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-protocol-handler.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/tunnel-bridge.hpp"
#endif

namespace aeronet::internal {

namespace {

constexpr std::string_view kTooManyPendingTunnels = "Too many CONNECT targets being resolved";
constexpr std::string_view kResolutionTimedOut = "CONNECT target resolution timed out";

}  // namespace

TunnelManager::TunnelManager(ServerContext& ctx) : _ctx(ctx), _resolver([&ctx] { ctx.lifecycle.wakeupFd.send(); }) {}

TunnelManager& TunnelManager::Of(ServerContext& ctx) {
  if (ctx.tunnels == nullptr) {
    ctx.tunnels = std::make_unique<TunnelManager>(ctx);
  }
  return *ctx.tunnels;
}

// ============================================================================
// HTTP/1.1 tunnels
// ============================================================================

TunnelManager::LoopAction TunnelManager::startHttp1Tunnel(ConnectionIt& cnxIt, std::size_t consumedBytes) {
  ConnectionState& state = _ctx.connections.connectionState(cnxIt);

  // authority-form requires a numeric port (RFC 9110 §9.3.6 / RFC 3986 port = *DIGIT). Reject anything
  // else (empty, non-numeric, or > 65535) up front with 400 instead of handing it to the resolver.
  const ConnectTarget target(state.request.path());
  if (target.invalid()) {
    _ctx.http1.emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Malformed CONNECT target");
    return LoopAction::Break;
  }

  // CONNECT is disabled unless the target is explicitly allowlisted or unrestricted access was requested with "*".
  if (!_ctx.config.connectTargetAllowed(target.host)) {
    _ctx.http1.emitSimpleError(cnxIt, http::StatusCodeForbidden, "CONNECT target not allowed");
    return LoopAction::Break;
  }

  // Save client fd - connectUpstream may invalidate the platform-specific connection iterator.
  const auto clientFd = cnxIt->fd();

  if (!IsNumericHost(target.host)) {
    // The connection reads nothing more until the resolution completes: a close of the client read with its request
    // would not be reported by any event anymore (edge-triggered polling).
    if (IsPeerClosed(clientFd)) {
      state.requestDrainAndClose();
      return LoopAction::Break;
    }
    if (_pendingTunnels.size() >= kMaxPendingTunnels) {
      _ctx.http1.emitSimpleError(cnxIt, http::StatusCodeServiceUnavailable, kTooManyPendingTunnels);
      return LoopAction::Break;
    }
    // Resolving a host name can block for seconds: it runs in the background. Until it completes, the connection reads
    // nothing and keeps the request in its input buffer (see completeHttp1Tunnel()).
    resolveTarget(clientFd, 0, consumedBytes, target.host, target.port);
    state.tunnelResolving = true;
    return LoopAction::SwitchProtocol;
  }

  const auto upstreamFd = connectUpstream(clientFd, target.host, target.port);

  // Re-find client iterator after potential connections reallocation inside connectUpstream.
  cnxIt = _ctx.connections.iterator(clientFd);
  assert(cnxIt != _ctx.connections.end() && "Client connection cannot vanish during connection insertion");

  if (upstreamFd == kInvalidHandle) {
    _ctx.http1.emitSimpleError(cnxIt, http::StatusCodeBadGateway, "Unable to establish CONNECT tunnel");
    return LoopAction::Break;
  }

  establishHttp1Tunnel(cnxIt, upstreamFd);

  // From now on, both connections bypass HTTP parsing. Keep any bytes following the CONNECT head so that they are
  // forwarded as tunnel data.
  return LoopAction::SwitchProtocol;
}

void TunnelManager::establishHttp1Tunnel(ConnectionIt cnxIt, NativeHandle upstreamFd) {
  ConnectionState& state = _ctx.connections.connectionState(cnxIt);

  _ctx.maintenance.clearRequestDeadline(state);
  state.headerStartTp = {};

  // A 2xx response to CONNECT has no content: the tunneled bytes directly follow its head (RFC 9110 §9.3.6).
  _ctx.io.queueData(cnxIt, HttpMessageData(BuildTunnelEstablished(_ctx.config.globalHeaders, _ctx.dateHeader.data())));
  _ctx.http1.endRequest(state.request, http::StatusCodeOK);

  // Enter tunneling mode: link client -> upstream (upstream -> client is set by registerUpstream).
  state.peerFd = upstreamFd;
  _ctx.maintenance.refreshKeepAliveDeadline(cnxIt);
}

void TunnelManager::completeHttp1Tunnel(const PendingTunnel& pending) {
  const NativeHandle upstreamFd = connectUpstream(pending);
  if (upstreamFd == kInvalidHandle) {
    refuseHttp1Tunnel(pending, http::StatusCodeBadGateway, "Unable to establish CONNECT tunnel");
    return;
  }

  // The connection insertion may have moved the client connection state.
  auto cnxIt = _ctx.connections.iterator(pending.clientFd);
  ConnectionState& state = _ctx.connections.connectionState(cnxIt);
  state.tunnelResolving = false;
  establishHttp1Tunnel(cnxIt, upstreamFd);
  // The request was kept in the input buffer, where the request views point to, until answered.
  state.inBuffer.erase_front(pending.consumedBytes);

  // Forward the tunnel bytes received with the request, and read those left in the socket while resolving.
  if (relayInput(cnxIt) == CloseStatus::Close) {
    _ctx.io.closeConnection(_ctx.connections.iterator(pending.clientFd));
    return;
  }
  cnxIt = _ctx.connections.iterator(pending.clientFd);
  if (_ctx.connections.connectionState(cnxIt).canCloseConnectionForDrain()) {
    _ctx.io.closeConnection(cnxIt);
  }
}

void TunnelManager::refuseHttp1Tunnel(const PendingTunnel& pending, http::StatusCode statusCode,
                                      std::string_view reason) {
  const auto cnxIt = _ctx.connections.iterator(pending.clientFd);
  assert(IsValid(_ctx.connections, cnxIt));  // its close forgets its pending tunnel
  ConnectionState& state = _ctx.connections.connectionState(cnxIt);
  state.tunnelResolving = false;
  _ctx.http1.emitSimpleError(cnxIt, statusCode, reason);
  // The request was kept in the input buffer, where the request views point to, until answered.
  state.inBuffer.erase_front(pending.consumedBytes);
  if (state.canCloseConnectionForDrain()) {
    _ctx.io.closeConnection(cnxIt);
  }
}

CloseStatus TunnelManager::relayInput(ConnectionIt cnxIt) {
  const auto selfFd = cnxIt->fd();
  ConnectionState& state = _ctx.connections.connectionState(cnxIt);
  // Backpressure: while a whole output buffer waits for the peer, the bytes of this endpoint stay in its socket, which
  // slows its sender down (TCP flow control). The relay resumes once the peer wrote them (see resumeRelaysTowards()).
  if (const auto peerIt = _ctx.connections.iterator(state.peerFd); IsValid(_ctx.connections, peerIt)) {
    ConnectionState& peerState = _ctx.connections.connectionState(peerIt);
    if (peerState.tunnelOrFileBuffer.size() >= _ctx.config.maxOutboundBufferBytes) {
      peerState.relayPausedByBacklog = true;
      return CloseStatus::Keep;
    }
  }
  std::size_t bytesReadThisEvent = 0;
  bool hitEagain = false;
  if (readInput(cnxIt, bytesReadThisEvent, hitEagain) == CloseStatus::Close) {
    return CloseStatus::Close;
  }

  if (state.inBuffer.empty()) {
    if (state.eofReceived) {
      auto peerIt = _ctx.connections.iterator(state.peerFd);
      if (IsValid(_ctx.connections, peerIt) && shutdownWrite(peerIt)) {
        return CloseStatus::Keep;  // peer closed and cleaned up
      }
      // Stop reading from this side — wait for the peer to close or drain.
      if (!_ctx.eventLoop.mod(EventLoop::EventFd{selfFd, EventOut | EventRdHup | EventEt})) [[unlikely]] {
        return CloseStatus::Close;
      }
    }
    return CloseStatus::Keep;
  }

  auto peerIt = _ctx.connections.iterator(state.peerFd);
  if (!IsValid(_ctx.connections, peerIt)) [[unlikely]] {
    return CloseStatus::Close;
  }

  if (!forward(peerIt, state.inBuffer)) [[unlikely]] {
    // Fatal transport error while forwarding to peer: close both sides.
    return CloseStatus::Close;
  }

  if (state.eofReceived) {
    if (shutdownWrite(peerIt)) {
      return CloseStatus::Keep;  // already cleaned up
    }
    if (!_ctx.eventLoop.mod(EventLoop::EventFd{selfFd, EventOut | EventRdHup | EventEt})) [[unlikely]] {
      return CloseStatus::Close;
    }
  }
  return CloseStatus::Keep;
}

// ============================================================================
// Shared by HTTP/1.1 and HTTP/2 tunnels
// ============================================================================

NativeHandle TunnelManager::connectUpstream(NativeHandle clientFd, std::string_view host, uint16_t port) {
  assert(IsNumericHost(host));
  ConnectResult cres = ConnectTCP(std::span<char>(const_cast<char*>(host.data()), host.size()), port);
  if (cres.failure) {
    return kInvalidHandle;
  }
  return registerUpstream(clientFd, std::move(cres));
}

NativeHandle TunnelManager::connectUpstream(const PendingTunnel& pending) {
  if (!pending.addresses) {
    return kInvalidHandle;
  }
  ConnectResult cres = ConnectTCP(*pending.addresses);
  if (cres.failure) {
    return kInvalidHandle;
  }
  return registerUpstream(pending.clientFd, std::move(cres));
}

NativeHandle TunnelManager::registerUpstream(NativeHandle clientFd, ConnectResult&& cres) {
  assert(!cres.failure);
  const auto upstreamFd = cres.cnx.fd();

  // Register upstream in event loop for edge-triggered reads and writes so we can detect
  // completion of non-blocking connect (EPOLLOUT) as well as incoming data.
  if (!_ctx.eventLoop.add(EventLoop::EventFd{upstreamFd, EventIn | EventOut | EventRdHup | EventEt})) [[unlikely]] {
    return kInvalidHandle;
  }

  // Insert upstream connection state. Insertion may invalidate connection iterators by growing the POSIX fd-indexed
  // vector or rehashing the Windows map, so callers must not hold them across this call. A duplicate fd for a newly
  // connected socket indicates a library bug (the kernel assigns unique fds for each socket()).
  const auto upIt = _ctx.connections.emplace(std::move(cres.cnx));
  ConnectionState& state = _ctx.connections.connectionState(upIt);

  // Set upstream transport to plain (no TLS). Tunneled bytes are relayed from reused buffers, with copying writes only
  // (never MSG_ZEROCOPY, whose kernel reads outlive the write).
  state.transport = Transport(upstreamFd);
  state.peerFd = clientFd;
  state.connectPending = cres.connectPending;

  return upstreamFd;
}

void TunnelManager::resolveTarget(NativeHandle clientFd, uint32_t streamId, std::size_t consumedBytes,
                                  std::string_view host, uint16_t port) {
  auto pending = std::make_shared<PendingTunnel>(host, port, clientFd, streamId, consumedBytes,
                                                 _ctx.connections.now + _ctx.config.keepAliveTimeout);
  _pendingTunnels.push_back(pending);
  _resolver.resolve(std::move(pending));
}

void TunnelManager::expirePendingTunnels(std::chrono::steady_clock::time_point now) {
  if (_pendingTunnels.empty()) {
    return;
  }
  vector<std::shared_ptr<PendingTunnel>> expired;
  const auto [first, last] =
      std::ranges::remove_if(_pendingTunnels, [now, &expired](const std::shared_ptr<PendingTunnel>& pending) {
        if (pending->deadline > now) {
          return false;
        }
        expired.push_back(pending);
        return true;
      });
  _pendingTunnels.erase(first, last);

  for (const auto& pending : expired) {
    // Its resolution, if in progress, cannot be interrupted: its result will be ignored.
    pending->cancelled = true;
    _resolver.cancel(pending);
    log::warn("CONNECT target {}:{} not resolved within the keep-alive timeout", pending->host(), pending->port);
#ifdef AERONET_ENABLE_HTTP2
    if (pending->streamId != 0) {
      const auto cnxIt = _ctx.connections.iterator(pending->clientFd);
      assert(IsValid(_ctx.connections, cnxIt));  // its close forgets its pending tunnels
      auto* h2Handler =
          static_cast<http2::Http2ProtocolHandler*>(_ctx.connections.connectionState(cnxIt).protocolHandler.get());
      if (h2Handler->isTunnelPending(pending->streamId)) {  // not reset meanwhile
        h2Handler->failPendingTunnel(pending->streamId, http::StatusCodeGatewayTimeout, kResolutionTimedOut);
        _ctx.io.flushOutbound(cnxIt);
      }
      continue;
    }
#endif
    refuseHttp1Tunnel(*pending, http::StatusCodeGatewayTimeout, kResolutionTimedOut);
  }
}

void TunnelManager::completeResolvedTunnels() {
  vector<std::shared_ptr<PendingTunnel>> resolved;
  _resolver.takeResolved(resolved);
  for (const auto& pending : resolved) {
    if (pending->cancelled) {
      continue;  // its client connection closed meanwhile
    }
    const auto pendingIt = std::ranges::find(_pendingTunnels, pending);
    assert(pendingIt != _pendingTunnels.end());
    _pendingTunnels.erase(pendingIt);
#ifdef AERONET_ENABLE_HTTP2
    if (pending->streamId != 0) {
      completeH2Tunnel(*pending);
      continue;
    }
#endif
    completeHttp1Tunnel(*pending);
  }
}

CloseStatus TunnelManager::handleReadable(ConnectionIt cnxIt) {
  const ConnectionState& state = _ctx.connections.connectionState(cnxIt);
  if (state.tunnelResolving) {
    // Nothing is read until the CONNECT target is resolved: the tunnel then reads the bytes left in the socket. A
    // client leaving meanwhile is closed right away, which cancels the resolution.
    return IsPeerClosed(cnxIt->fd()) ? CloseStatus::Close : CloseStatus::Keep;
  }
#ifdef AERONET_ENABLE_HTTP2
  if (state.peerStreamId != 0) {
    return relayToStream(cnxIt);
  }
#endif
  return relayInput(cnxIt);
}

CloseStatus TunnelManager::handleWritable(ConnectionIt cnxIt) {
  ConnectionState& state = _ctx.connections.connectionState(cnxIt);
  const auto fd = cnxIt->fd();

  // The non-blocking connect to an upstream completed: SO_ERROR tells whether it succeeded.
  if (state.connectPending) {
    const int err = GetSocketError(fd);
    state.connectPending = false;
    if (err != 0) {
      // Upstream connect failed. Attempt to notify the client side (peerFd) and close this upstream.
      const auto peerIt = _ctx.connections.iterator(state.peerFd);
      if (IsValid(_ctx.connections, peerIt)) {
#ifdef AERONET_ENABLE_HTTP2
        if (state.peerStreamId != 0) {
          // HTTP/2 tunnel upstream: RST_STREAM the tunnel stream
          ConnectionState& peerState = _ctx.connections.connectionState(peerIt);
          auto* h2Handler = static_cast<http2::Http2ProtocolHandler*>(peerState.protocolHandler.get());
          h2Handler->tunnelConnectFailed(state.peerStreamId);
          _ctx.io.flushOutbound(peerIt);
        } else
#endif
        {
          _ctx.http1.emitSimpleError(peerIt, http::StatusCodeBadGateway, "Upstream connect failed");
        }
      } else {
        log::error("Unable to notify client of upstream connect failure: peer fd # {} not found", state.peerFd);
      }
      return CloseStatus::Close;
    }
  }

  // Write the tunneled bytes buffered for this endpoint.
  if (!state.tunnelOrFileBuffer.empty() && !state.tunnelTransportWrite(fd)) {
    return CloseStatus::Close;
  }
  if (state.relayPausedByBacklog && state.tunnelOrFileBuffer.empty()) {
    resumeRelaysTowards(cnxIt);
  }
  return CloseStatus::Keep;
}

void TunnelManager::resumeRelaysTowards(ConnectionIt cnxIt) {
  ConnectionState& state = _ctx.connections.connectionState(cnxIt);
  state.relayPausedByBacklog = false;
#ifdef AERONET_ENABLE_HTTP2
  if (state.peerStreamId != 0) {
    // The upstream of an HTTP/2 tunnel: its client stream gets flow-control credit again.
    const auto peerIt = _ctx.connections.iterator(state.peerFd);
    if (IsValid(_ctx.connections, peerIt)) {
      ConnectionState& peerState = _ctx.connections.connectionState(peerIt);
      static_cast<http2::Http2ProtocolHandler*>(peerState.protocolHandler.get())->onTunnelUpstreamDrained(cnxIt->fd());
      _ctx.io.flushOutbound(peerIt);
    }
    return;
  }
  if (state.protocolHandler && state.protocolHandler->type() == ProtocolType::Http2) {
    // The client connection of HTTP/2 tunnels wrote its output: their upstreams are read again.
    const auto* h2Handler = static_cast<const http2::Http2ProtocolHandler*>(state.protocolHandler.get());
    for (const auto& [upstreamFd, streamId] : h2Handler->tunnelUpstreams()) {
      const auto upIt = _ctx.connections.iterator(upstreamFd);
      if (IsValid(_ctx.connections, upIt)) {
        _ctx.io.deferInput(upstreamFd, _ctx.connections.connectionState(upIt));
      }
    }
    return;
  }
#endif
  // An endpoint of an HTTP/1.1 tunnel: its peer is read again. Deferred to the next event-loop iteration: no event
  // reports the bytes left in its socket.
  const auto peerIt = _ctx.connections.iterator(state.peerFd);
  if (IsValid(_ctx.connections, peerIt)) {
    _ctx.io.deferInput(state.peerFd, _ctx.connections.connectionState(peerIt));
  }
}

void TunnelManager::closeTunnelsOf(ConnectionIt cnxIt) {
  const auto fd = cnxIt->fd();
  ConnectionState& state = _ctx.connections.connectionState(cnxIt);

  if (!_pendingTunnels.empty()) {
    const auto [first, last] =
        std::ranges::remove_if(_pendingTunnels, [this, fd](const std::shared_ptr<PendingTunnel>& pending) {
          if (pending->clientFd != fd) {
            return false;
          }
          pending->cancelled = true;
          _resolver.cancel(pending);
          return true;
        });
    _pendingTunnels.erase(first, last);
  }

  // Tear down the peer of a tunnel endpoint too. Otherwise, peerFd may dangle and later accidentally match a reused fd,
  // causing spurious epoll_ctl failures and incorrect forwarding.
  if (state.peerFd != kInvalidHandle) {
    auto peerIt = _ctx.connections.iterator(state.peerFd);
    if (IsValid(_ctx.connections, peerIt)) [[likely]] {
      ConnectionState& peerState = _ctx.connections.connectionState(peerIt);
#ifdef AERONET_ENABLE_HTTP2
      if (state.peerStreamId != 0) {
        // HTTP/2 tunnel upstream being closed: notify the peer's handler to send END_STREAM,
        // but do NOT tear down the peer HTTP/2 connection (it may have other active streams).
        if (peerState.protocolHandler) {
          static_cast<http2::Http2ProtocolHandler*>(peerState.protocolHandler.get())->closeTunnelByUpstreamFd(fd);
          _ctx.io.flushOutbound(peerIt);
        }
      } else
#endif
          if (peerState.peerFd == fd) [[likely]] {
        _ctx.maintenance.forgetConnectionMaintenance(peerState);
        _ctx.io.releaseConnection(peerIt);
      } else {
        log::error("Tunnel peer mismatch while closing fd # {} (peerFd={}, peer.peerFd={})", fd, state.peerFd,
                   peerState.peerFd);
      }
    }
  }

#ifdef AERONET_ENABLE_HTTP2
  // The upstreams of the tunnels of an HTTP/2 connection. Their peerFd is reset so that they do not try to close the
  // connection again.
  if (state.protocolHandler && state.protocolHandler->type() == ProtocolType::Http2) {
    auto* h2Handler = static_cast<http2::Http2ProtocolHandler*>(state.protocolHandler.get());
    for (const auto& [upFd, streamId] : h2Handler->drainTunnelUpstreamFds()) {
      auto upIt = _ctx.connections.iterator(upFd);
      if (IsValid(_ctx.connections, upIt)) {
        ConnectionState& upState = _ctx.connections.connectionState(upIt);
        upState.peerFd = kInvalidHandle;
        upState.peerStreamId = 0;
        _ctx.maintenance.forgetConnectionMaintenance(upState);
        _ctx.io.releaseConnection(upIt);
      }
    }
  }
#endif
}

bool TunnelManager::forward(ConnectionIt targetIt, std::string_view data) {
  ConnectionState& target = _ctx.connections.connectionState(targetIt);

  // If the target is still connecting, waiting for EPOLLOUT, or has buffered data, just buffer.
  if (target.connectPending || target.waitingWritable || !target.tunnelOrFileBuffer.empty()) {
    target.tunnelOrFileBuffer.append(data);
    return target.waitingWritable || _ctx.io.enableWritableInterest(targetIt);
  }

  // Attempt direct write.
  const auto [written, want] = target.transportWrite(data);
  if (want == TransportHint::Error) [[unlikely]] {
    return false;
  }

  // Buffer any unwritten remainder.
  if (static_cast<std::size_t>(written) < data.size()) {
    target.tunnelOrFileBuffer.append(data.data() + written, data.size() - written);
    return target.waitingWritable || _ctx.io.enableWritableInterest(targetIt);
  }
  return true;
}

bool TunnelManager::forward(ConnectionIt targetIt, RawChars& sourceBuffer) {
  ConnectionState& target = _ctx.connections.connectionState(targetIt);

  // Buffer the bytes not written yet, swapping the buffers when possible to avoid a copy.
  const auto bufferRemainder = [&] {
    if (target.tunnelOrFileBuffer.empty()) {
      sourceBuffer.swap(target.tunnelOrFileBuffer);
    } else {
      target.tunnelOrFileBuffer.append(sourceBuffer);
      sourceBuffer.clear();
    }
    return target.waitingWritable || _ctx.io.enableWritableInterest(targetIt);
  };

  // If the target is still connecting, waiting for EPOLLOUT, or has buffered data, just buffer.
  if (target.connectPending || target.waitingWritable || !target.tunnelOrFileBuffer.empty()) {
    return bufferRemainder();
  }

  // Attempt direct write.
  const auto [written, want] = target.transportWrite(std::string_view(sourceBuffer));
  if (want == TransportHint::Error) [[unlikely]] {
    return false;
  }
  sourceBuffer.erase_front(written);
  return sourceBuffer.empty() || bufferRemainder();
}

bool TunnelManager::shutdownWrite(ConnectionIt peerIt) {
  ConnectionState& peer = _ctx.connections.connectionState(peerIt);
  peer.shutdownWritePending = true;
  if (peer.tunnelOrFileBuffer.empty()) {
    if (!ShutdownWrite(peerIt->fd())) {
      log::warn("Failed to shutdown write for peer fd # {}", peerIt->fd());
      _ctx.io.closeConnection(peerIt);
      return true;  // peerIt and its peer are now recycled — do not touch state
    }
    peer.shutdownWritePending = false;
  }
  return false;
}

CloseStatus TunnelManager::readInput(ConnectionIt cnxIt, std::size_t& bytesReadThisEvent, bool& hitEagain) {
  ConnectionState& state = _ctx.connections.connectionState(cnxIt);
  const HttpServerConfig& config = _ctx.config;
  while (!state.eofReceived && state.inBuffer.size() < config.maxOutboundBufferBytes) {
    const std::size_t chunkSize = config.computeReadChunkSize(bytesReadThisEvent, state.readSizeShift);
    assert(chunkSize > 0);
    const auto [bytesRead, want] = state.transportRead(config, chunkSize);
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
    if (bytesRead < chunkSize && !state.transport.hasPendingReadData()) {
      hitEagain = true;
      break;
    }
    if (config.fairnessBudgetExhausted(bytesReadThisEvent)) {
      // Edge-triggered polling (EPOLLET): data may remain in the TCP buffer after the fairness cap.
      // No new read event fires on a non-empty→non-empty transition, so defer this fd for
      // re-processing at the start of the next event-loop iteration (same as handleReadableClient).
      _ctx.io.deferInput(cnxIt->fd(), state);
      hitEagain = true;
      break;
    }
  }
  return CloseStatus::Keep;
}

// ============================================================================
// HTTP/2 tunnels
// ============================================================================

#ifdef AERONET_ENABLE_HTTP2

// Forwards the tunnel operations of the HTTP/2 handler of a client connection to the TunnelManager of its server,
// created by the first one.
class TunnelManager::H2Bridge final : public ITunnelBridge {
 public:
  H2Bridge(ServerContext& ctx, NativeHandle clientFd) noexcept : _ctx(ctx), _clientFd(clientFd) {}

  TunnelSetup setupTunnel(uint32_t streamId, std::string_view host, uint16_t port) override {
    return Of(_ctx).startH2Tunnel(_clientFd, streamId, host, port);
  }

  bool writeTunnel(NativeHandle upstreamFd, std::span<const std::byte> data) override {
    return Of(_ctx).writeToUpstream(upstreamFd, data);
  }

  void shutdownTunnelWrite(NativeHandle upstreamFd) override { Of(_ctx).shutdownUpstreamWrite(upstreamFd); }

  void closeTunnel(NativeHandle upstreamFd) override { Of(_ctx).closeUpstream(upstreamFd); }

  void onTunnelWindowUpdate(NativeHandle upstreamFd) override { Of(_ctx).resumeRelayToStream(upstreamFd); }

 private:
  ServerContext& _ctx;
  NativeHandle _clientFd;
};

std::unique_ptr<ITunnelBridge> TunnelManager::MakeH2Bridge(ServerContext& ctx, NativeHandle clientFd) {
  return std::make_unique<H2Bridge>(ctx, clientFd);
}

ITunnelBridge::TunnelSetup TunnelManager::startH2Tunnel(NativeHandle clientFd, uint32_t streamId, std::string_view host,
                                                        uint16_t port) {
  ITunnelBridge::TunnelSetup setup;
  if (!IsNumericHost(host)) {
    if (_pendingTunnels.size() >= kMaxPendingTunnels) {
      setup.status = ITunnelBridge::TunnelSetup::Status::Overloaded;
      return setup;
    }
    // Resolving a host name can block for seconds: it runs in the background, see completeH2Tunnel().
    resolveTarget(clientFd, streamId, 0, host, port);
    setup.status = ITunnelBridge::TunnelSetup::Status::Pending;
    return setup;
  }
  setup.upstreamFd = connectUpstream(clientFd, host, port);
  if (setup.upstreamFd != kInvalidHandle) {
    linkH2Upstream(setup.upstreamFd, streamId);
    setup.status = ITunnelBridge::TunnelSetup::Status::Established;
  }
  return setup;
}

void TunnelManager::completeH2Tunnel(const PendingTunnel& pending) {
  auto* h2Handler = static_cast<http2::Http2ProtocolHandler*>(
      _ctx.connections.pConnectionState(pending.clientFd)->protocolHandler.get());
  if (!h2Handler->isTunnelPending(pending.streamId)) {
    return;  // the stream was reset or closed meanwhile
  }
  const NativeHandle upstreamFd = connectUpstream(pending);
  if (upstreamFd != kInvalidHandle) {
    linkH2Upstream(upstreamFd, pending.streamId);
  } else {
    log::warn("HTTP/2 CONNECT stream {} failed to connect to {}:{}", pending.streamId, pending.host(), pending.port);
  }
  // The connection insertion may have moved the client connection state.
  ConnectionState& clientState = *_ctx.connections.pConnectionState(pending.clientFd);
  static_cast<http2::Http2ProtocolHandler*>(clientState.protocolHandler.get())
      ->onTunnelResolved(pending.streamId, upstreamFd);
  _ctx.io.flushOutbound(_ctx.connections.iterator(pending.clientFd));
}

void TunnelManager::linkH2Upstream(NativeHandle upstreamFd, uint32_t streamId) {
  auto upIt = _ctx.connections.iterator(upstreamFd);
  assert(upIt != _ctx.connections.end() && *upIt);
  _ctx.connections.connectionState(upIt).peerStreamId = streamId;
}

CloseStatus TunnelManager::relayToStream(ConnectionIt cnxIt) {
  ConnectionState& state = _ctx.connections.connectionState(cnxIt);

  // Find the client HTTP/2 connection via peerFd.
  auto peerIt = _ctx.connections.iterator(state.peerFd);
  if (!IsValid(_ctx.connections, peerIt)) [[unlikely]] {
    return CloseStatus::Close;
  }

  ConnectionState& peerState = _ctx.connections.connectionState(peerIt);
  auto* pH2Handler = static_cast<http2::Http2ProtocolHandler*>(peerState.protocolHandler.get());
  if (pH2Handler == nullptr) [[unlikely]] {
    return CloseStatus::Close;
  }

  auto* stream = pH2Handler->connection().getStream(state.peerStreamId);
  if (stream == nullptr) {
    return CloseStatus::Close;
  }

  bool hitEagain = false;
  std::size_t bytesReadThisEvent = 0;

  while (true) {
    // Backpressure: the flow-control windows are granted by the client, which may keep granting credit without
    // reading. While its connection has a whole output buffer to write, the upstream bytes stay in the socket (see
    // resumeRelaysTowards()).
    if (pH2Handler->connection().pendingOutputSize() >= _ctx.config.maxOutboundBufferBytes) {
      peerState.relayPausedByBacklog = true;
      return CloseStatus::Keep;
    }

    // Read from upstream in a loop (edge-triggered, must drain), but respect flow control.
    // If inBuffer is already large, don't read more until we can inject it.
    if (readInput(cnxIt, bytesReadThisEvent, hitEagain) == CloseStatus::Close) {
      return CloseStatus::Close;
    }

    if (state.inBuffer.empty()) {
      return state.eofReceived ? CloseStatus::Close : CloseStatus::Keep;
    }

    // Determine how much we can inject based on HTTP/2 flow control windows.
    int32_t streamWin = stream->sendWindow();
    int32_t connWin = pH2Handler->connection().connectionSendWindow();
    int32_t win = std::min(streamWin, connWin);

    if (win <= 0) {
      // Wait for WINDOW_UPDATE. The windowUpdate callback will re-invoke this function.
      return CloseStatus::Keep;
    }

    const auto injectSize = std::min(state.inBuffer.size(), static_cast<RawChars::size_type>(win));

    // Inject data as HTTP/2 DATA frame(s) on the tunnel stream.
    const auto data = std::as_bytes(std::span<const char>(state.inBuffer.data(), injectSize));
    const auto err = pH2Handler->injectTunnelData(state.peerStreamId, data);

    state.inBuffer.erase_front(injectSize);

    if (err != http2::ErrorCode::NoError) [[unlikely]] {
      log::warn("HTTP/2 CONNECT stream {} inject failed: {}", state.peerStreamId, http2::ErrorCodeName(err));
      return CloseStatus::Close;
    }

    // Flush the HTTP/2 handler's output through the client connection.
    if (pH2Handler->hasPendingOutput()) {
      _ctx.io.flushOutbound(peerIt);
    }

    // If we hit EAGAIN, we are done for now.
    if (hitEagain) {
      break;
    }
    // If we didn't hit EAGAIN, but we injected some data, we can loop and read more.
    // If we didn't inject anything (win <= 0), we would have returned above.
  }
  return CloseStatus::Keep;
}

bool TunnelManager::writeToUpstream(NativeHandle upstreamFd, std::span<const std::byte> data) {
  auto upIt = _ctx.connections.iterator(upstreamFd);
  // The handler forgets the upstream of a stream when it closes (closeTunnelsOf() and drainTunnelUpstreamFds()).
  assert(IsValid(_ctx.connections, upIt));
  ConnectionState& upState = _ctx.connections.connectionState(upIt);
  if (!forward(upIt, std::string_view(reinterpret_cast<const char*>(data.data()), data.size()))) [[unlikely]] {
    _ctx.io.closeConnection(upIt);
  } else if (!upState.tunnelOrFileBuffer.empty()) {
    // The client stream gets no more flow-control credit until these bytes are written (see resumeRelaysTowards()).
    upState.relayPausedByBacklog = true;
    return false;
  }
  return true;
}

void TunnelManager::shutdownUpstreamWrite(NativeHandle upstreamFd) {
  auto upIt = _ctx.connections.iterator(upstreamFd);
  if (IsValid(_ctx.connections, upIt)) {
    shutdownWrite(upIt);
  }
}

void TunnelManager::closeUpstream(NativeHandle upstreamFd) {
  auto upIt = _ctx.connections.iterator(upstreamFd);
  if (!IsValid(_ctx.connections, upIt)) {
    return;
  }
  ConnectionState& state = _ctx.connections.connectionState(upIt);
  // Clear peerFd so closeConnection won't try to tear down the client HTTP/2 connection.
  state.peerFd = kInvalidHandle;
  state.peerStreamId = 0;
  _ctx.io.closeConnection(upIt);
}

void TunnelManager::resumeRelayToStream(NativeHandle upstreamFd) {
  auto upIt = _ctx.connections.iterator(upstreamFd);
  if (!IsValid(_ctx.connections, upIt)) {
    return;
  }
  // If we have buffered data from upstream, try to inject it now that the window opened.
  const ConnectionState& state = _ctx.connections.connectionState(upIt);
  if ((!state.inBuffer.empty() || state.eofReceived) && relayToStream(upIt) == CloseStatus::Close) {
    _ctx.io.closeConnection(_ctx.connections.iterator(upstreamFd));
  }
}

#endif

}  // namespace aeronet::internal
