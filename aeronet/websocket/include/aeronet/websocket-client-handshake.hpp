#pragma once

#include <array>
#include <string_view>

#include "aeronet/websocket-config.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/websocket-deflate.hpp"
#endif

namespace aeronet::websocket {

/// A Sec-WebSocket-Key value: the base64 encoding of 16 random bytes (RFC 6455 §4.1).
using WebSocketKey = std::array<char, 24>;

/// A new Sec-WebSocket-Key, from the secure random generator of the system: each opening handshake uses a new one.
[[nodiscard]] WebSocketKey GenerateWebSocketKey();

/// The header field values of a 101 (Switching Protocols) response that the client checks, empty when absent.
struct UpgradeResponseHeaders {
  std::string_view upgrade;
  std::string_view connection;
  std::string_view accept;
  std::string_view protocol;
  std::string_view extensions;
};

/// Outcome of ValidateUpgradeResponse().
struct UpgradeResponseValidation {
  [[nodiscard]] bool valid() const noexcept { return pErrorMessage == nullptr; }

#ifdef AERONET_ENABLE_ZLIB
  /// The negotiated permessage-deflate parameters, empty when the server did not accept the offer.
  DeflateNegotiatedParams deflateParams;
#endif

  /// Why the client fails the connection, null when the response is valid.
  const char* pErrorMessage{nullptr};
};

/// Checks the 101 response of the server to an opening handshake (RFC 6455 §4.1): Upgrade is websocket, Connection
/// holds upgrade, Sec-WebSocket-Accept is computed from 'key', Sec-WebSocket-Protocol is absent or one of
/// 'offeredProtocols' (the comma-separated Sec-WebSocket-Protocol value of the request, empty if none), and
/// Sec-WebSocket-Extensions only accepts the permessage-deflate offer made from config.deflateConfig, if enabled.
[[nodiscard]] UpgradeResponseValidation ValidateUpgradeResponse(const UpgradeResponseHeaders& headers,
                                                                std::string_view key, std::string_view offeredProtocols,
                                                                const WebSocketConfig& config);

}  // namespace aeronet::websocket
