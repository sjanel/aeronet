#pragma once

#include <string_view>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"

#ifdef AERONET_ENABLE_WEBSOCKET
#include <optional>

#include "aeronet/websocket-deflate.hpp"
#include "aeronet/websocket-upgrade.hpp"
#endif

namespace aeronet {

/// Result of validating an HTTP Upgrade request.
struct UpgradeValidationResult {
  bool valid{false};
  ProtocolType targetProtocol{ProtocolType::Http11};

#ifdef AERONET_ENABLE_WEBSOCKET
  B64EncodedSha1 secWebSocketAccept;  // Computed Sec-WebSocket-Accept value

  // Negotiated WebSocket permessage-deflate parameters (if compression was negotiated)
  std::optional<websocket::DeflateNegotiatedParams> deflateParams;
#endif

  std::string_view errorMessage;  // Populated if !valid

  // WebSocket-specific fields (populated when targetProtocol == WebSocket)
  std::string_view selectedProtocol;  // Selected subprotocol (if any)

  // Offered protocols by the client (empty if none offered)
  ConcatenatedStrings offeredProtocols;

  // Offered extensions by the client (empty if none offered)
  ConcatenatedStrings offeredExtensions;
};

/// Utility functions for protocol upgrade handling.
///
/// This module provides validation and response generation for:
///   - WebSocket upgrades (RFC 6455)
///   - HTTP/2 cleartext upgrades (h2c, RFC 9113 §3.2)
///
/// For HTTP/2 over TLS (h2), ALPN negotiation is used instead of Upgrade.
namespace upgrade {

/// Check if a Connection header value contains "upgrade" (case-insensitive).
///
/// The Connection header may contain multiple comma-separated tokens.
/// This function checks if any of them is "upgrade".
///
/// @param connectionValue  The value of the Connection header
/// @return                 True if "upgrade" token is present
[[nodiscard]] bool ConnectionContainsUpgrade(std::string_view connectionValue);

#ifdef AERONET_ENABLE_WEBSOCKET
/// Check if the request contains an Upgrade header requesting WebSocket.
///
/// Validates:
///   - Upgrade: websocket (case-insensitive)
///   - Connection: upgrade (case-insensitive, may contain other tokens)
///   - Sec-WebSocket-Version: 13
///   - Sec-WebSocket-Key: present and 24 bytes (base64 of 16 random bytes)
///
/// @param headers  Map of HTTP request headers
/// @param config   Optional configuration for subprotocol/extension negotiation
/// @return         Validation result with computed Sec-WebSocket-Accept if valid
[[nodiscard]] UpgradeValidationResult ValidateWebSocketUpgrade(const HeadersViewMap& headers,
                                                               const WebSocketUpgradeConfig& config);
#endif

#ifdef AERONET_ENABLE_HTTP2
/// Check if the request contains an Upgrade header requesting HTTP/2 (h2c).
///
/// Validates:
///   - Upgrade: h2c
///   - Connection: Upgrade, HTTP2-Settings
///   - HTTP2-Settings header present (base64url encoded SETTINGS frame payload)
///
/// @param headers  Map of HTTP request headers
/// @return         Validation result
[[nodiscard]] UpgradeValidationResult ValidateHttp2Upgrade(const HeadersViewMap& headers);
#endif

/// Detect the upgrade target from an HTTP request.
///
/// Examines the Upgrade header and returns the target protocol.
/// Does NOT perform full validation - use ValidateWebSocketUpgrade() or
/// ValidateHttp2Upgrade() for complete validation.
///
/// @param request  The incoming HTTP request
/// @return         Target protocol type, or Http11 if no valid upgrade requested
[[nodiscard]] ProtocolType DetectUpgradeTarget(std::string_view upgradeHeaderValue);

#ifdef AERONET_ENABLE_WEBSOCKET
/// Generate a raw 101 Switching Protocols response for WebSocket upgrade.
///
/// Returns the complete HTTP response as raw bytes, ready to be written to the socket.
/// This bypasses HttpResponse because 101 responses require setting reserved headers
/// (Connection, Upgrade) which normal response building disallows.
///
/// @param validationResult  Result from ValidateWebSocketUpgrade() (must be valid)
/// @return                  Complete 101 response as raw bytes
[[nodiscard]] RawChars BuildWebSocketUpgradeResponse(const UpgradeValidationResult& validationResult);
#endif

#ifdef AERONET_ENABLE_HTTP2
/// Generate a raw 101 Switching Protocols response for HTTP/2 upgrade.
///
/// Returns the complete HTTP response as raw bytes, ready to be written to the socket.
/// Note: After sending this response, the server must immediately send the
/// HTTP/2 connection preface (SETTINGS frame), and then respond to the
/// original request using HTTP/2.
///
/// @param validationResult  Result from ValidateHttp2Upgrade() (must be valid)
/// @return                  Complete 101 response as raw bytes
[[nodiscard]] std::string_view BuildHttp2UpgradeResponse(const UpgradeValidationResult& validationResult);
#endif

}  // namespace upgrade

}  // namespace aeronet
