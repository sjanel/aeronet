#include <cassert>

#include "aeronet/connection-state.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-protocol-handler.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"

namespace aeronet {

void SingleHttpServer::installHttp2Handler(NativeHandle clientFd, ConnectionState& state,
                                           bool sendServerPrefaceForTls) {
  state.protocolHandler = http2::CreateHttp2ProtocolHandler(_ctx, state, clientFd, sendServerPrefaceForTls);
  // Async resumption, stream deadline sweeps and the HTTP/2 connection count all key off the protocol.
  _ctx.maintenance.trackHttp2Connection();
  state.protocol = ProtocolType::Http2;
  // The header read timer bounds the reception of an HTTP/1.1 request head: the one started for the request expected
  // since the accept (or since the first bytes of the preface) does not apply to the HTTP/2 connection.
  _ctx.maintenance.stopHeaderReadTimer(state);
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
