#include "aeronet/connection-state.hpp"
#include "aeronet/http2-protocol-handler.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/single-http-server.hpp"

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

}  // namespace aeronet
