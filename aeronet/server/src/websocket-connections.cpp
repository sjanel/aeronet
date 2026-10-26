#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/http1-pipeline.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/upgrade-handler.hpp"
#include "aeronet/websocket-endpoint.hpp"
#include "aeronet/websocket-handler.hpp"  // IWYU pragma: keep (handler of the accept result, moved to the connection)

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
  ServerContext& ctx = *_pCtx;
  upgrade::WebSocketAcceptResult accept = upgrade::AcceptWebSocketUpgrade(
      request, request.headers(), *routingResult.webSocketEndpoint(), ctx.webSocketDeflateStreams);
  switch (accept.status) {
    case upgrade::WebSocketAcceptResult::Status::Accepted:
      break;
    case upgrade::WebSocketAcceptResult::Status::Invalid:
      emitSimpleError(cnxIt, http::StatusCodeBadRequest, accept.pErrorMessage);
      return LoopAction::Break;  // the connection closes
    case upgrade::WebSocketAcceptResult::Status::Refused:
      sendResponse(cnxIt, request.makeResponse(http::StatusCodeForbidden), routingResult, kStreaming);
      return LoopAction::Continue;
    default:
      sendResponse(cnxIt, request.makeResponse(http::StatusCodeInternalServerError), routingResult, kStreaming);
      return LoopAction::Continue;
  }

  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  // The per-route request timeout bounds the upgrade request, not the WebSocket connection that follows.
  ctx.maintenance.clearRequestDeadline(state);

  state.protocolHandler = std::move(accept.handler);
  state.protocol = ProtocolType::WebSocket;
  ctx.io.queueData(cnxIt, HttpMessageData(std::move(accept.response)));

  // From now on, the WebSocket idle and close timeouts apply instead of keepAliveTimeout.
  ctx.maintenance.refreshWebSocketDeadline(cnxIt->fd(), state);
  return LoopAction::SwitchProtocol;
}

}  // namespace aeronet::internal
