#pragma once

#include <cstdint>
#include <string_view>

namespace aeronet {

// Application protocol spoken over a client connection. Today only HTTP/1.1 is implemented; HTTP/2 is
// reserved so the connection pool / ALPN negotiation can already carry the distinction (the HTTP/2
// protocol engine, reusing the server's HPACK + frame codecs, slots in beside the HTTP/1.1 one later).
enum class ClientProtocol : uint8_t {
  Http1_1,  // ALPN "http/1.1" (default, the only protocol with a handler today)
  Http2,    // ALPN "h2" (reserved -- no handler yet)
};

// ALPN protocol identifier advertised / negotiated for a ClientProtocol.
[[nodiscard]] constexpr std::string_view ToAlpnId(ClientProtocol protocol) noexcept {
  switch (protocol) {
    case ClientProtocol::Http2:
      return "h2";
    default:
      return "http/1.1";
  }
}

// Map a negotiated ALPN identifier back to a ClientProtocol. An unknown or empty selection (e.g. a server
// that does not speak ALPN) falls back to HTTP/1.1.
[[nodiscard]] constexpr ClientProtocol ClientProtocolFromAlpnId(std::string_view alpn) noexcept {
  if (alpn == "h2") {
    return ClientProtocol::Http2;
  }
  return ClientProtocol::Http1_1;
}

// Whether a protocol can multiplex several concurrent exchanges (streams) over a single connection.
// HTTP/1.1 cannot (strictly one exchange at a time); HTTP/2 can. The connection pool consults this so it
// never bakes in a 1:1 connection<->request assumption.
[[nodiscard]] constexpr bool ProtocolSupportsMultiplexing(ClientProtocol protocol) noexcept {
  return protocol == ClientProtocol::Http2;
}

}  // namespace aeronet
