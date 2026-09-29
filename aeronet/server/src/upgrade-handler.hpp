#pragma once

#include <string_view>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/sv-to-sv-map.hpp"

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
/// This module provides validation and response generation for WebSocket upgrades (RFC 6455).
///
/// `Upgrade: h2c` is not supported on purpose: RFC 9113 §3.1 deprecated it, so such requests are answered over
/// HTTP/1.1. HTTP/2 is reached through ALPN "h2" over TLS, or with prior knowledge over cleartext.
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
[[nodiscard]] UpgradeValidationResult ValidateWebSocketUpgrade(const SvToSvMap& headers,
                                                               const WebSocketUpgradeConfig& config);
#endif

/// Detect the upgrade target from an HTTP request.
///
/// Examines the Upgrade header and returns the target protocol.
/// Does NOT perform full validation - use ValidateWebSocketUpgrade() for complete validation.
///
/// @param upgradeHeaderValue  Value of the Upgrade request header (empty if absent)
/// @return                    Target protocol type, or Http11 if no supported upgrade is requested (including "h2c")
[[nodiscard]] ProtocolType DetectUpgradeTarget(std::string_view upgradeHeaderValue);

#ifdef AERONET_ENABLE_WEBSOCKET

std::size_t ComputeWebSocketUpgradeResponseSize(const UpgradeValidationResult& validationResult);

/// Generate a raw 101 Switching Protocols response for WebSocket upgrade.
///
/// Returns the complete HTTP response as raw bytes, ready to be written to the socket.
/// This bypasses HttpResponse because 101 responses require setting reserved headers
/// (Connection, Upgrade) which normal response building disallows.
///
/// @param validationResult  Result from ValidateWebSocketUpgrade() (must be valid)
void BuildWebSocketUpgradeResponse(const UpgradeValidationResult& validationResult, char* pData);
#endif

}  // namespace upgrade

}  // namespace aeronet
