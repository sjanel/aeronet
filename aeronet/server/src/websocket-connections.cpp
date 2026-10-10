#include <chrono>
#include <exception>
#include <memory>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/keep-alive-deadline-queue.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/router.hpp"
#include "aeronet/single-http-server.hpp"

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-endpoint.hpp"
#include "aeronet/websocket-handler.hpp"
#include "aeronet/websocket-upgrade.hpp"
#include "upgrade-handler.hpp"
#endif

namespace aeronet {

#ifdef AERONET_ENABLE_WEBSOCKET

SingleHttpServer::LoopAction SingleHttpServer::processWebSocketUpgrade(ConnectionIt cnxIt,
                                                                       const Router::RoutingResult& routingResult) {
  static constexpr bool kStreaming = false;
  if (rejectDeniedCorsOrigin(cnxIt, routingResult, kStreaming)) {
    return LoopAction::Continue;
  }
  const HttpRequestView& request = _connections.connectionState(cnxIt).request;
  // Browsers do not apply CORS to WebSocket: without CORS policy, only same-origin pages may connect, which
  // prevents cross-site WebSocket hijacking. Clients that are not browsers send no Origin.
  if (routingResult.corsPolicy() == nullptr &&
      !upgrade::IsSameOriginRequest(request.headerValueOrEmpty(http::Origin), request.headerValueOrEmpty(http::Host))) {
    sendHttp1Response(cnxIt,
                      request.makeResponse(http::StatusCodeForbidden, "Cross-origin WebSocket connection refused"),
                      routingResult, kStreaming);
    return LoopAction::Continue;
  }
  switch (upgradeToWebSocket(cnxIt, *routingResult.webSocketEndpoint())) {
    case WebSocketUpgrade::Upgraded:
      return LoopAction::SwitchProtocol;
    case WebSocketUpgrade::Invalid:
      return LoopAction::Break;  // answered 400, the connection closes
    case WebSocketUpgrade::Refused:
      sendHttp1Response(cnxIt, request.makeResponse(http::StatusCodeForbidden), routingResult, kStreaming);
      return LoopAction::Continue;
    default:
      sendHttp1Response(cnxIt, request.makeResponse(http::StatusCodeInternalServerError), routingResult, kStreaming);
      return LoopAction::Continue;
  }
}

SingleHttpServer::WebSocketUpgrade SingleHttpServer::upgradeToWebSocket(ConnectionIt cnxIt,
                                                                        const WebSocketEndpoint& endpoint) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  const HttpRequestView& request = state.request;

  const WebSocketUpgradeConfig upgradeConfig{endpoint.supportedProtocols, endpoint.config.deflateConfig};
  const auto upgradeValidation = upgrade::ValidateWebSocketUpgrade(request.headers(), upgradeConfig);
  if (!upgradeValidation.valid) {
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, upgradeValidation.errorMessage);
    return WebSocketUpgrade::Invalid;
  }

  // Create WebSocket handler using the endpoint's factory or default
  std::unique_ptr<websocket::WebSocketHandler> wsHandler;
  if (endpoint.factory) {
    try {
      wsHandler = endpoint.factory(request);
    } catch (const std::exception& ex) {
      log::error("Exception in WebSocket handler factory: {}", ex.what());
      return WebSocketUpgrade::Failed;
    } catch (...) {
      log::error("Unknown exception in WebSocket handler factory");
      return WebSocketUpgrade::Failed;
    }
    if (!wsHandler) {
      return WebSocketUpgrade::Refused;
    }
    if (!wsHandler->hasCompression() && upgradeValidation.deflateParams.has_value()) {
      // Compression was negotiated but the factory did not configure it: enable it on the handler, keeping
      // the callbacks the factory installed.
      wsHandler->enableCompression(*upgradeValidation.deflateParams);
    }
  } else {
    auto config = endpoint.config;
    config.isServerSide = true;
    wsHandler = std::make_unique<websocket::WebSocketHandler>(config, websocket::WebSocketCallbacks(),
                                                              upgradeValidation.deflateParams);
  }

  // The per-route request timeout bounds the upgrade request, not the WebSocket connection that follows.
  clearRequestDeadline(state);

  // Install the protocol handler
  state.protocolHandler = std::move(wsHandler);
  state.protocol = ProtocolType::WebSocket;

  // Queue the 101 Switching Protocols response
  char* pData = state.outBuffer.resizeUp(upgrade::ComputeWebSocketUpgradeResponseSize(upgradeValidation));
  upgrade::BuildWebSocketUpgradeResponse(upgradeValidation, pData);
  flushOutbound(cnxIt);

  // From now on, the WebSocket idle and close timeouts apply instead of keepAliveTimeout.
  refreshWebSocketDeadline(cnxIt->fd(), state);
  return WebSocketUpgrade::Upgraded;
}

void SingleHttpServer::refreshWebSocketDeadline(NativeHandle fd, ConnectionState& state) {
  const auto& handler = static_cast<const websocket::WebSocketHandler&>(*state.protocolHandler);
  const auto nextCheck = handler.nextTimeoutCheck(state.lastActivity);
  if (nextCheck == std::chrono::steady_clock::time_point::max()) {
    _keepAliveDeadlines.remove(state);
    return;
  }
  // Like for keep-alive, an armed deadline earlier than the next check is kept: lastActivity only moves forward, so
  // the check re-arms it when it fires. Only the start of a close handshake can bring the next check closer.
  if (!internal::KeepAliveDeadlineQueue::contains(state) || nextCheck < _keepAliveDeadlines.expiresAt(state)) {
    _keepAliveDeadlines.upsert(state, fd, nextCheck);
  }
}

bool SingleHttpServer::checkWebSocketTimeouts(ConnectionIt cnxIt) {
  const NativeHandle fd = cnxIt->fd();
  ConnectionState& state = _connections.connectionState(cnxIt);
  auto& handler = static_cast<websocket::WebSocketHandler&>(*state.protocolHandler);
  const auto timeout = handler.checkTimeouts(_connections.now, state.lastActivity);
  if (timeout != websocket::WebSocketHandler::Timeout::None) {
    const bool idle = timeout == websocket::WebSocketHandler::Timeout::Idle;
    log::debug("sweepIdleConnections: fd # {} closed for WebSocket {} timeout", fd, idle ? "idle" : "close");
    closeConnection(cnxIt);
    _telemetry.counterAdd(idle ? "aeronet.connections.closed_for_websocket_idle_timeout"
                               : "aeronet.connections.closed_for_websocket_close_timeout");
    return true;
  }
  if (handler.hasPendingOutput()) {
    flushOutbound(cnxIt);  // the Ping of an idle connection
  }
  refreshWebSocketDeadline(fd, state);
  return false;
}

#endif

}  // namespace aeronet
