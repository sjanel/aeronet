#include <cassert>
#include <cstring>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#endif

#include "aeronet/connection-state.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-protocol-handler.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "tunnel-manager.hpp"

namespace aeronet {

void SingleHttpServer::installHttp2Handler(NativeHandle clientFd, ConnectionState& state,
                                           bool sendServerPrefaceForTls) {
  state.protocolHandler = http2::CreateHttp2ProtocolHandler(
      _ctx.config.http2, _ctx.requestPipeline(), _ctx.config, _ctx.compressionState, _ctx.decompressionState,
      sendServerPrefaceForTls, _ctx.dateHeader.data(), state, internal::TunnelManager::MakeH2Bridge(_ctx, clientFd));
  // Async resumption, stream deadline sweeps and the HTTP/2 connection count all key off the protocol.
  _ctx.maintenance.trackHttp2Connection();
  state.protocol = ProtocolType::Http2;
  // The header read timer bounds the reception of an HTTP/1.1 request head: the one started for the request expected
  // since the accept (or since the first bytes of the preface) does not apply to the HTTP/2 connection.
  _ctx.maintenance.stopHeaderReadTimer(state);

  auto* h2Handler = static_cast<http2::Http2ProtocolHandler*>(state.protocolHandler.get());

  // Install per-request completion callback for metrics, counters and tracing.
  h2Handler->setRequestCompletionCallback(
      [pCtx = &_ctx, fd = clientFd](const HttpRequestView& request, http::StatusCode status) {
        auto& state = *pCtx->connections.pConnectionState(fd);
        ++state.requestsServed;
        ++pCtx->stats.totalRequestsServed;
        if (pCtx->callbacks.metrics || pCtx->accessLog) {
          pCtx->emitRequestMetrics(request, status);
        }
      });

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Install async callback so HTTP/2 coroutines can post deferred work to the event loop.
  h2Handler->setAsyncPostCallback(
      [pCtx = &_ctx, fd = clientFd, generation = state.generation](std::coroutine_handle<> handle) {
        pCtx->postAsyncCallback(fd, generation, handle);
      });
#endif
}

void SingleHttpServer::setupHttp2Connection(NativeHandle clientFd, TcpNoDelayMode tcpNoDelayMode,
                                            ConnectionState& state) {
  // The server must send SETTINGS immediately for TLS ALPN "h2".
  installHttp2Handler(clientFd, state, true);

  if (tcpNoDelayMode == TcpNoDelayMode::Auto) {
    assert(state.tlsInfo.selectedAlpn() == http2::kAlpnH2);
    // Disable Nagle's algorithm for HTTP/2 connections by default to reduce latency.
    // The protocol handler may choose to re-enable it later if it determines it's beneficial.
    if (SetTcpNoDelay(clientFd)) [[likely]] {
      state.corkable = true;
    } else {
      const auto err = LastSystemError();
      log::error("setsockopt(TCP_NODELAY) failed for fd # {} err={}", clientFd, err);
      _ctx.telemetry.counterAdd("aeronet.connections.errors.tcp_nodelay_failed", 1UL);
    }
  }

  // The queued server preface is flushed by the caller through the protocol gather path.
}

}  // namespace aeronet
