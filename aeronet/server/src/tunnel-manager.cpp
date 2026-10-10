#include "tunnel-manager.hpp"

#include <algorithm>
#include <cassert>
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
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zerocopy-mode.hpp"
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

TunnelManager::TunnelManager(SingleHttpServer& server)
    : _server(server), _resolver([&server] { server._lifecycle.wakeupFd.send(); }) {}

TunnelManager& TunnelManager::Of(SingleHttpServer& server) {
  if (server._tunnels == nullptr) {
    server._tunnels = std::make_unique<TunnelManager>(server);
  }
  return *server._tunnels;
}

// ============================================================================
// HTTP/1.1 tunnels
// ============================================================================

TunnelManager::LoopAction TunnelManager::startHttp1Tunnel(ConnectionIt& cnxIt, std::size_t consumedBytes) {
  ConnectionState& state = _server._connections.connectionState(cnxIt);

  // authority-form requires a numeric port (RFC 9110 §9.3.6 / RFC 3986 port = *DIGIT). Reject anything
  // else (empty, non-numeric, or > 65535) up front with 400 instead of handing it to the resolver.
  const ConnectTarget target(state.request.path());
  if (target.invalid()) {
    _server.emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Malformed CONNECT target");
    return LoopAction::Break;
  }

  // CONNECT is disabled unless the target is explicitly allowlisted or unrestricted access was requested with "*".
  if (!_server._config.connectTargetAllowed(target.host)) {
    _server.emitSimpleError(cnxIt, http::StatusCodeForbidden, "CONNECT target not allowed");
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
    // Resolving a host name can block for seconds: it runs in the background. Until it completes, the connection reads
    // nothing and keeps the request in its input buffer (see completeHttp1Tunnel()).
    resolveTarget(clientFd, 0, consumedBytes, target.host, target.port);
    state.tunnelResolving = true;
    return LoopAction::SwitchProtocol;
  }

  const auto upstreamFd = connectUpstream(clientFd, target.host, target.port);

  // Re-find client iterator after potential connections reallocation inside connectUpstream.
  cnxIt = _server._connections.iterator(clientFd);
  assert(cnxIt != _server._connections.end() && "Client connection cannot vanish during connection insertion");

  if (upstreamFd == kInvalidHandle) {
    _server.emitSimpleError(cnxIt, http::StatusCodeBadGateway, "Unable to establish CONNECT tunnel");
    return LoopAction::Break;
  }

  establishHttp1Tunnel(cnxIt, upstreamFd);

  // From now on, both connections bypass HTTP parsing. Keep any bytes following the CONNECT head so that they are
  // forwarded as tunnel data.
  return LoopAction::SwitchProtocol;
}

void TunnelManager::establishHttp1Tunnel(ConnectionIt cnxIt, NativeHandle upstreamFd) {
  ConnectionState& state = _server._connections.connectionState(cnxIt);

  _server.clearRequestDeadline(state);
  state.headerStartTp = {};

  // A 2xx response to CONNECT has no content: the tunneled bytes directly follow its head (RFC 9110 §9.3.6).
  _server.queueData(cnxIt,
                    HttpMessageData(BuildTunnelEstablished(_server._config.globalHeaders, _server._dateHeader.data())));
  _server.endRequest(state.request, http::StatusCodeOK, state.requestsServed > 0);

  // Enter tunneling mode: link client -> upstream (upstream -> client is set by registerUpstream).
  state.peerFd = upstreamFd;
  _server.refreshKeepAliveDeadline(cnxIt);

  // Disable zerocopy on the client-side transport for the same buffer lifetime reason.
  state.transport.disableZerocopy();
}

void TunnelManager::completeHttp1Tunnel(const PendingTunnel& pending) {
  const NativeHandle upstreamFd = connectUpstream(pending);

  // The connection insertion may have moved the client connection state.
  auto cnxIt = _server._connections.iterator(pending.clientFd);
  ConnectionState& state = _server._connections.connectionState(cnxIt);
  state.tunnelResolving = false;
  if (upstreamFd == kInvalidHandle) {
    _server.emitSimpleError(cnxIt, http::StatusCodeBadGateway, "Unable to establish CONNECT tunnel");
  } else {
    establishHttp1Tunnel(cnxIt, upstreamFd);
  }
  // The request was kept in the input buffer, where the request views point to, until answered.
  state.inBuffer.erase_front(pending.consumedBytes);

  // Forward the tunnel bytes received with the request, and read those left in the socket while resolving.
  if (upstreamFd != kInvalidHandle && relayInput(cnxIt) == CloseStatus::Close) {
    _server.closeConnection(_server._connections.iterator(pending.clientFd));
    return;
  }
  cnxIt = _server._connections.iterator(pending.clientFd);
  if (_server._connections.connectionState(cnxIt).canCloseConnectionForDrain()) {
    _server.closeConnection(cnxIt);
  }
}

TunnelManager::CloseStatus TunnelManager::relayInput(ConnectionIt cnxIt) {
  const auto selfFd = cnxIt->fd();
  ConnectionState& state = _server._connections.connectionState(cnxIt);
  std::size_t bytesReadThisEvent = 0;
  bool hitEagain = false;
  if (readInput(cnxIt, bytesReadThisEvent, hitEagain) == CloseStatus::Close) {
    return CloseStatus::Close;
  }

  if (state.inBuffer.empty()) {
    if (state.eofReceived) {
      auto peerIt = _server._connections.iterator(state.peerFd);
      if (IsValid(_server._connections, peerIt) && shutdownWrite(peerIt)) {
        return CloseStatus::Keep;  // peer closed and cleaned up
      }
      // Stop reading from this side — wait for the peer to close or drain.
      if (!_server._eventLoop.mod(EventLoop::EventFd{selfFd, EventOut | EventRdHup | EventEt})) [[unlikely]] {
        return CloseStatus::Close;
      }
    }
    return CloseStatus::Keep;
  }

  auto peerIt = _server._connections.iterator(state.peerFd);
  if (!IsValid(_server._connections, peerIt)) [[unlikely]] {
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
    if (!_server._eventLoop.mod(EventLoop::EventFd{selfFd, EventOut | EventRdHup | EventEt})) [[unlikely]] {
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
  if (!_server._eventLoop.add(EventLoop::EventFd{upstreamFd, EventIn | EventOut | EventRdHup | EventEt})) [[unlikely]] {
    return kInvalidHandle;
  }

  // Insert upstream connection state. Insertion may invalidate connection iterators by growing the POSIX fd-indexed
  // vector or rehashing the Windows map, so callers must not hold them across this call. A duplicate fd for a newly
  // connected socket indicates a library bug (the kernel assigns unique fds for each socket()).
  const auto upIt = _server._connections.emplace(std::move(cres.cnx));
  ConnectionState& state = _server._connections.connectionState(upIt);

  // Set upstream transport to plain (no TLS). Zerocopy is unconditionally disabled for tunnel
  // transports because buffer lifetimes are not stable — data is read into a reusable inBuffer
  // and forwarded immediately; the kernel may still have pages pinned for DMA when the buffer is
  // reused for the next read, causing data corruption.
  state.transport = Transport(upstreamFd, ZerocopyMode::Disabled, 0);
  state.peerFd = clientFd;
  state.connectPending = cres.connectPending;

  return upstreamFd;
}

void TunnelManager::resolveTarget(NativeHandle clientFd, uint32_t streamId, std::size_t consumedBytes,
                                  std::string_view host, uint16_t port) {
  auto pending = std::make_shared<PendingTunnel>(host, port, clientFd, streamId, consumedBytes);
  _pendingTunnels.push_back(pending);
  _resolver.resolve(std::move(pending));
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

TunnelManager::CloseStatus TunnelManager::handleReadable(ConnectionIt cnxIt) {
  const ConnectionState& state = _server._connections.connectionState(cnxIt);
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

TunnelManager::CloseStatus TunnelManager::handleWritable(ConnectionIt cnxIt) {
  ConnectionState& state = _server._connections.connectionState(cnxIt);
  const auto fd = cnxIt->fd();

  // The non-blocking connect to an upstream completed: SO_ERROR tells whether it succeeded.
  if (state.connectPending) {
    const int err = GetSocketError(fd);
    state.connectPending = false;
    if (err != 0) {
      // Upstream connect failed. Attempt to notify the client side (peerFd) and close this upstream.
      const auto peerIt = _server._connections.iterator(state.peerFd);
      if (IsValid(_server._connections, peerIt)) {
#ifdef AERONET_ENABLE_HTTP2
        if (state.peerStreamId != 0) {
          // HTTP/2 tunnel upstream: RST_STREAM the tunnel stream
          ConnectionState& peerState = _server._connections.connectionState(peerIt);
          auto* h2Handler = static_cast<http2::Http2ProtocolHandler*>(peerState.protocolHandler.get());
          h2Handler->tunnelConnectFailed(state.peerStreamId);
          _server.flushOutbound(peerIt);
        } else
#endif
        {
          _server.emitSimpleError(peerIt, http::StatusCodeBadGateway, "Upstream connect failed");
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
  return CloseStatus::Keep;
}

void TunnelManager::closeTunnelsOf(ConnectionIt cnxIt) {
  const auto fd = cnxIt->fd();
  ConnectionState& state = _server._connections.connectionState(cnxIt);

  if (!_pendingTunnels.empty()) {
    const auto [first, last] =
        std::ranges::remove_if(_pendingTunnels, [fd](const std::shared_ptr<PendingTunnel>& pending) {
          if (pending->clientFd != fd) {
            return false;
          }
          pending->cancelled = true;
          return true;
        });
    _pendingTunnels.erase(first, last);
  }

  // Tear down the peer of a tunnel endpoint too. Otherwise, peerFd may dangle and later accidentally match a reused fd,
  // causing spurious epoll_ctl failures and incorrect forwarding.
  if (state.peerFd != kInvalidHandle) {
    auto peerIt = _server._connections.iterator(state.peerFd);
    if (IsValid(_server._connections, peerIt)) [[likely]] {
      ConnectionState& peerState = _server._connections.connectionState(peerIt);
#ifdef AERONET_ENABLE_HTTP2
      if (state.peerStreamId != 0) {
        // HTTP/2 tunnel upstream being closed: notify the peer's handler to send END_STREAM,
        // but do NOT tear down the peer HTTP/2 connection (it may have other active streams).
        if (peerState.protocolHandler) {
          static_cast<http2::Http2ProtocolHandler*>(peerState.protocolHandler.get())->closeTunnelByUpstreamFd(fd);
          _server.flushOutbound(peerIt);
        }
      } else
#endif
          if (peerState.peerFd == fd) [[likely]] {
        _server.forgetConnectionMaintenance(peerState);
        _server.releaseConnection(peerIt);
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
      auto upIt = _server._connections.iterator(upFd);
      if (IsValid(_server._connections, upIt)) {
        ConnectionState& upState = _server._connections.connectionState(upIt);
        upState.peerFd = kInvalidHandle;
        upState.peerStreamId = 0;
        _server.forgetConnectionMaintenance(upState);
        _server.releaseConnection(upIt);
      }
    }
  }
#endif
}

bool TunnelManager::forward(ConnectionIt targetIt, std::string_view data) {
  ConnectionState& target = _server._connections.connectionState(targetIt);

  // If the target is still connecting, waiting for EPOLLOUT, or has buffered data, just buffer.
  if (target.connectPending || target.waitingWritable || !target.tunnelOrFileBuffer.empty()) {
    target.tunnelOrFileBuffer.append(data);
    return target.waitingWritable || _server.enableWritableInterest(targetIt);
  }

  // Attempt direct write.
  const auto [written, want] = target.transportWrite(data);
  if (want == TransportHint::Error) [[unlikely]] {
    return false;
  }

  // Buffer any unwritten remainder.
  if (static_cast<std::size_t>(written) < data.size()) {
    target.tunnelOrFileBuffer.append(data.data() + written, data.size() - written);
    return target.waitingWritable || _server.enableWritableInterest(targetIt);
  }
  return true;
}

bool TunnelManager::forward(ConnectionIt targetIt, RawChars& sourceBuffer) {
  ConnectionState& target = _server._connections.connectionState(targetIt);

  // Buffer the bytes not written yet, swapping the buffers when possible to avoid a copy.
  const auto bufferRemainder = [&] {
    if (target.tunnelOrFileBuffer.empty()) {
      sourceBuffer.swap(target.tunnelOrFileBuffer);
    } else {
      target.tunnelOrFileBuffer.append(sourceBuffer);
      sourceBuffer.clear();
    }
    return target.waitingWritable || _server.enableWritableInterest(targetIt);
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
  ConnectionState& peer = _server._connections.connectionState(peerIt);
  peer.shutdownWritePending = true;
  if (peer.tunnelOrFileBuffer.empty()) {
    if (!ShutdownWrite(peerIt->fd())) {
      log::warn("Failed to shutdown write for peer fd # {}", peerIt->fd());
      _server.closeConnection(peerIt);
      return true;  // peerIt and its peer are now recycled — do not touch state
    }
    peer.shutdownWritePending = false;
  }
  return false;
}

TunnelManager::CloseStatus TunnelManager::readInput(ConnectionIt cnxIt, std::size_t& bytesReadThisEvent,
                                                    bool& hitEagain) {
  ConnectionState& state = _server._connections.connectionState(cnxIt);
  const HttpServerConfig& config = _server._config;
  while (!state.eofReceived && state.inBuffer.size() < config.maxOutboundBufferBytes) {
    const std::size_t chunkSize = config.computeReadChunkSize(bytesReadThisEvent);
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
    if (bytesRead < chunkSize && !state.transport.hasPendingReadData()) {
      hitEagain = true;
      break;
    }
    if (config.fairnessBudgetExhausted(bytesReadThisEvent)) {
      // Edge-triggered polling (EPOLLET): data may remain in the TCP buffer after the fairness cap.
      // No new read event fires on a non-empty→non-empty transition, so defer this fd for
      // re-processing at the start of the next event-loop iteration (same as handleReadableClient).
      _server.deferInput(cnxIt->fd(), state);
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
  H2Bridge(SingleHttpServer& server, NativeHandle clientFd) noexcept : _server(server), _clientFd(clientFd) {}

  TunnelSetup setupTunnel(uint32_t streamId, std::string_view host, uint16_t port) override {
    return Of(_server).startH2Tunnel(_clientFd, streamId, host, port);
  }

  void writeTunnel(NativeHandle upstreamFd, std::span<const std::byte> data) override {
    Of(_server).writeToUpstream(upstreamFd, data);
  }

  void shutdownTunnelWrite(NativeHandle upstreamFd) override { Of(_server).shutdownUpstreamWrite(upstreamFd); }

  void closeTunnel(NativeHandle upstreamFd) override { Of(_server).closeUpstream(upstreamFd); }

  void onTunnelWindowUpdate(NativeHandle upstreamFd) override { Of(_server).resumeRelayToStream(upstreamFd); }

 private:
  SingleHttpServer& _server;
  NativeHandle _clientFd;
};

std::unique_ptr<ITunnelBridge> TunnelManager::MakeH2Bridge(SingleHttpServer& server, NativeHandle clientFd) {
  return std::make_unique<H2Bridge>(server, clientFd);
}

ITunnelBridge::TunnelSetup TunnelManager::startH2Tunnel(NativeHandle clientFd, uint32_t streamId, std::string_view host,
                                                        uint16_t port) {
  ITunnelBridge::TunnelSetup setup;
  if (!IsNumericHost(host)) {
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
      _server._connections.pConnectionState(pending.clientFd)->protocolHandler.get());
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
  ConnectionState& clientState = *_server._connections.pConnectionState(pending.clientFd);
  static_cast<http2::Http2ProtocolHandler*>(clientState.protocolHandler.get())
      ->onTunnelResolved(pending.streamId, upstreamFd);
  _server.flushOutbound(_server._connections.iterator(pending.clientFd));
}

void TunnelManager::linkH2Upstream(NativeHandle upstreamFd, uint32_t streamId) {
  auto upIt = _server._connections.iterator(upstreamFd);
  assert(upIt != _server._connections.end() && *upIt);
  _server._connections.connectionState(upIt).peerStreamId = streamId;
}

TunnelManager::CloseStatus TunnelManager::relayToStream(ConnectionIt cnxIt) {
  ConnectionState& state = _server._connections.connectionState(cnxIt);

  // Find the client HTTP/2 connection via peerFd.
  auto peerIt = _server._connections.iterator(state.peerFd);
  if (!IsValid(_server._connections, peerIt)) [[unlikely]] {
    return CloseStatus::Close;
  }

  ConnectionState& peerState = _server._connections.connectionState(peerIt);
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
      _server.flushOutbound(peerIt);
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

void TunnelManager::writeToUpstream(NativeHandle upstreamFd, std::span<const std::byte> data) {
  auto upIt = _server._connections.iterator(upstreamFd);
  if (!IsValid(_server._connections, upIt)) {
    return;
  }
  if (!forward(upIt, std::string_view(reinterpret_cast<const char*>(data.data()), data.size()))) [[unlikely]] {
    _server.closeConnection(upIt);
  }
}

void TunnelManager::shutdownUpstreamWrite(NativeHandle upstreamFd) {
  auto upIt = _server._connections.iterator(upstreamFd);
  if (IsValid(_server._connections, upIt)) {
    shutdownWrite(upIt);
  }
}

void TunnelManager::closeUpstream(NativeHandle upstreamFd) {
  auto upIt = _server._connections.iterator(upstreamFd);
  if (!IsValid(_server._connections, upIt)) {
    return;
  }
  ConnectionState& state = _server._connections.connectionState(upIt);
  // Clear peerFd so closeConnection won't try to tear down the client HTTP/2 connection.
  state.peerFd = kInvalidHandle;
  state.peerStreamId = 0;
  _server.closeConnection(upIt);
}

void TunnelManager::resumeRelayToStream(NativeHandle upstreamFd) {
  auto upIt = _server._connections.iterator(upstreamFd);
  if (!IsValid(_server._connections, upIt)) {
    return;
  }
  // If we have buffered data from upstream, try to inject it now that the window opened.
  const ConnectionState& state = _server._connections.connectionState(upIt);
  if ((!state.inBuffer.empty() || state.eofReceived) && relayToStream(upIt) == CloseStatus::Close) {
    _server.closeConnection(_server._connections.iterator(upstreamFd));
  }
}

#endif

}  // namespace aeronet::internal
