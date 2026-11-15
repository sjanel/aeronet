#pragma once

#include <array>
#include <string_view>

#include "aeronet/concatenated-strings.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/websocket-deflate.hpp"
#endif

namespace aeronet {

// Base64-encoded SHA-1 is always 28 chars
using B64EncodedSha1 = std::array<char, 28>;

/// Configuration for WebSocket upgrade validation.
struct WebSocketUpgradeConfig {
  /// Subprotocols supported by the server, in order of preference.
  /// If the client offers one of these, the first matching one is selected.
  /// If empty, no subprotocol negotiation is performed.
  const ConcatenatedStrings& supportedProtocols;

#ifdef AERONET_ENABLE_ZLIB
  /// Deflate configuration (used when enableCompression is true).
  websocket::DeflateConfig deflateConfig;
#endif
};

/// Validate the format of a Sec-WebSocket-Key.
///
/// A valid key is exactly 24 base64 characters (representing 16 random bytes).
///
/// @param key  The value of the Sec-WebSocket-Key header
/// @return     True if the key format is valid
bool IsValidWebSocketKey(std::string_view key);

/// Compute the Sec-WebSocket-Accept value from a client's Sec-WebSocket-Key.
///
/// The algorithm (RFC 6455 §1.3):
///   1. Concatenate the key with the WebSocket GUID
///   2. Compute SHA-1 hash
///   3. Base64 encode the result
///
/// @param key  The value of the Sec-WebSocket-Key header
/// @return     The computed Sec-WebSocket-Accept value
B64EncodedSha1 ComputeWebSocketAccept(std::string_view key);

}  // namespace aeronet