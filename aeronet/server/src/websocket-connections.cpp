#include <cstddef>
#include <exception>
#include <memory>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/http1-pipeline.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/upgrade-handler.hpp"
#include "aeronet/websocket-endpoint.hpp"
#include "aeronet/websocket-handler.hpp"
#include "aeronet/websocket-upgrade.hpp"

namespace aeronet::internal {

Http1Pipeline::LoopAction Http1Pipeline::processWebSocketUpgrade(ConnectionIt cnxIt,
                                                                 const Router::RoutingResult& routingResult) {
  static constexpr bool kStreaming = false;
  const HttpRequestView& request = _pCtx->connections.connectionState(cnxIt).request;
  if (RequestPipeline::IsOriginDenied(request, routingResult)) {
    sendResponse(cnxIt, RequestPipeline::MakeOriginDenial(request), routingResult, kStreaming);
    return LoopAction::Continue;
  }
  // Browsers do not apply CORS to WebSocket: without CORS policy, only same-origin pages may connect, which
  // prevents cross-site WebSocket hijacking. Clients that are not browsers send no Origin.
  if (routingResult.corsPolicy() == nullptr &&
      !upgrade::IsSameOriginRequest(request.headerValueOrEmpty(http::Origin), request.headerValueOrEmpty(http::Host))) {
    sendResponse(cnxIt, request.makeResponse(http::StatusCodeForbidden, "Cross-origin WebSocket connection refused"),
                 routingResult, kStreaming);
    return LoopAction::Continue;
  }
  switch (upgradeToWebSocket(cnxIt, *routingResult.webSocketEndpoint())) {
    case WebSocketUpgrade::Upgraded:
      return LoopAction::SwitchProtocol;
    case WebSocketUpgrade::Invalid:
      return LoopAction::Break;  // answered 400, the connection closes
    case WebSocketUpgrade::Refused:
      sendResponse(cnxIt, request.makeResponse(http::StatusCodeForbidden), routingResult, kStreaming);
      return LoopAction::Continue;
    default:
      sendResponse(cnxIt, request.makeResponse(http::StatusCodeInternalServerError), routingResult, kStreaming);
      return LoopAction::Continue;
  }
}

Http1Pipeline::WebSocketUpgrade Http1Pipeline::upgradeToWebSocket(ConnectionIt cnxIt,
                                                                  const WebSocketEndpoint& endpoint) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  const HttpRequestView& request = state.request;

  const WebSocketUpgradeConfig upgradeConfig{endpoint.supportedProtocols, endpoint.config.deflateConfig};
  const auto upgradeValidation = upgrade::ValidateWebSocketUpgrade(request.headers(), upgradeConfig);
  if (!upgradeValidation.valid()) {
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, upgradeValidation.pErrorMessage);
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
  } else {
    auto config = endpoint.config;
    config.isServerSide = true;
    wsHandler = std::make_unique<websocket::WebSocketHandler>(std::move(config));
  }
  if (!wsHandler->hasCompression() && upgradeValidation.deflateParams.hasValue()) {
    // Compression was negotiated but the handler (from the factory, which keeps its callbacks) was created without it.
    // The directions without context takeover use the streams shared by the connections of this server thread.
    wsHandler->enableCompression(upgradeValidation.deflateParams, &ctx.webSocketDeflateStreams);
  }

  // The per-route request timeout bounds the upgrade request, not the WebSocket connection that follows.
  ctx.maintenance.clearRequestDeadline(state);

  // Install the protocol handler
  state.protocolHandler = std::move(wsHandler);
  state.protocol = ProtocolType::WebSocket;

  // Queue the 101 Switching Protocols response
  const std::size_t responseSize = upgrade::ComputeWebSocketUpgradeResponseSize(upgradeValidation);
  RawChars response(responseSize);
  upgrade::BuildWebSocketUpgradeResponse(upgradeValidation, response.data());
  response.setSize(responseSize);
  ctx.io.queueData(cnxIt, HttpMessageData(std::move(response)));

  // From now on, the WebSocket idle and close timeouts apply instead of keepAliveTimeout.
  ctx.maintenance.refreshWebSocketDeadline(cnxIt->fd(), state);
  return WebSocketUpgrade::Upgraded;
}

}  // namespace aeronet::internal
