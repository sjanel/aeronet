#pragma once

#include <cstdint>
#include <string_view>

namespace aeronet {

// Application protocol spoken over a client connection: HTTP/1.1 or HTTP/2 (native engine reusing the
// server's HPACK + frame codecs). The connection pool / ALPN negotiation carry the distinction per
// connection.
enum class ClientProtocol : uint8_t {
  Http1_1,  // ALPN "http/1.1" (default)
  Http2,    // ALPN "h2" (https) or h2c prior knowledge (plain http)
};

// Which HTTP version(s) HttpClient may speak, and how the version is chosen per connection
// (HttpClientConfig::httpVersion).
enum class HttpVersionMode : uint8_t {
  // Negotiate the best version the origin supports: https advertises "h2" + "http/1.1" via ALPN (the
  // upgrade to HTTP/2 is automatic when the server selects it); plain http stays HTTP/1.1 (RFC 9113
  // deprecated the cleartext Upgrade mechanism -- use Http2 for prior-knowledge h2c).
  Auto,
  // HTTP/1.1 only: never advertise nor speak HTTP/2.
  Http1_1,
  // HTTP/2 only: https advertises only "h2" via ALPN (an origin that does not select it fails the request
  // with HttpClientErrc::protocolUnsupported); plain http speaks h2c with prior knowledge (RFC 9113 §3.4).
  Http2,
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
