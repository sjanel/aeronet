#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/websocket-deflate.hpp"

namespace aeronet {

// Forward declarations
class HttpRequest;

/// Result of validating an HTTP Upgrade request.
struct UpgradeValidationResult {
  using B64EncodedSha1 = std::array<char, 28>;  // Base64-encoded SHA-1 is always 28 chars

  bool valid{false};
  ProtocolType targetProtocol{ProtocolType::Http11};
  B64EncodedSha1 secWebSocketAccept;  // Computed Sec-WebSocket-Accept value

  // Negotiated permessage-deflate parameters (if compression was negotiated)
  std::optional<websocket::DeflateNegotiatedParams> deflateParams;

  std::string_view errorMessage;  // Populated if !valid

  // WebSocket-specific fields (populated when targetProtocol == WebSocket)
  std::string selectedProtocol;  // Selected subprotocol (if any)

  // Offered protocols by the client (empty if none offered)
  ConcatenatedStrings offeredProtocols;

  // Offered extensions by the client (empty if none offered)
  ConcatenatedStrings offeredExtensions;
};

/// Configuration for WebSocket upgrade validation.
struct WebSocketUpgradeConfig {
  /// Subprotocols supported by the server, in order of preference.
  /// If the client offers one of these, the first matching one is selected.
  /// If empty, no subprotocol negotiation is performed.
  std::span<const std::string_view> supportedProtocols;

  /// Whether to enable permessage-deflate compression (RFC 7692).
  /// If true and the client offers permessage-deflate, it will be negotiated.
  bool enableCompression{false};

  /// Deflate configuration (used when enableCompression is true).
  websocket::DeflateConfig deflateConfig;
};

/// Utility functions for protocol upgrade handling.
///
/// This module provides validation and response generation for:
///   - WebSocket upgrades (RFC 6455)
///   - HTTP/2 cleartext upgrades (h2c, RFC 9113 §3.2)
///
/// For HTTP/2 over TLS (h2), ALPN negotiation is used instead of Upgrade.
namespace upgrade {

/// Check if the request contains an Upgrade header requesting WebSocket.
///
/// Validates:
///   - Upgrade: websocket (case-insensitive)
///   - Connection: upgrade (case-insensitive, may contain other tokens)
///   - Sec-WebSocket-Version: 13
///   - Sec-WebSocket-Key: present and 24 bytes (base64 of 16 random bytes)
///
/// @param request  The incoming HTTP request
/// @param config   Optional configuration for subprotocol/extension negotiation
/// @return         Validation result with computed Sec-WebSocket-Accept if valid
[[nodiscard]] UpgradeValidationResult ValidateWebSocketUpgrade(const HttpRequest& request,
                                                               const WebSocketUpgradeConfig& config = {});

/// Check if the request contains an Upgrade header requesting HTTP/2 (h2c).
///
/// Validates:
///   - Upgrade: h2c
///   - Connection: Upgrade, HTTP2-Settings
///   - HTTP2-Settings header present (base64url encoded SETTINGS frame payload)
///
/// @param request  The incoming HTTP request
/// @return         Validation result
[[nodiscard]] UpgradeValidationResult ValidateHttp2Upgrade(const HttpRequest& request);

/// Detect the upgrade target from an HTTP request.
///
/// Examines the Upgrade header and returns the target protocol.
/// Does NOT perform full validation - use ValidateWebSocketUpgrade() or
/// ValidateHttp2Upgrade() for complete validation.
///
/// @param request  The incoming HTTP request
/// @return         Target protocol type, or Http11 if no valid upgrade requested
[[nodiscard]] ProtocolType DetectUpgradeTarget(const HttpRequest& request);

/// Generate a raw 101 Switching Protocols response for WebSocket upgrade.
///
/// Returns the complete HTTP response as raw bytes, ready to be written to the socket.
/// This bypasses HttpResponse because 101 responses require setting reserved headers
/// (Connection, Upgrade) which normal response building disallows.
///
/// @param validationResult  Result from ValidateWebSocketUpgrade() (must be valid)
/// @return                  Complete 101 response as raw bytes
[[nodiscard]] RawChars BuildWebSocketUpgradeResponse(const UpgradeValidationResult& validationResult);

/// Generate a raw 101 Switching Protocols response for HTTP/2 upgrade.
///
/// Returns the complete HTTP response as raw bytes, ready to be written to the socket.
/// Note: After sending this response, the server must immediately send the
/// HTTP/2 connection preface (SETTINGS frame), and then respond to the
/// original request using HTTP/2.
///
/// @param validationResult  Result from ValidateHttp2Upgrade() (must be valid)
/// @return                  Complete 101 response as raw bytes
[[nodiscard]] RawChars BuildHttp2UpgradeResponse(const UpgradeValidationResult& validationResult);

/// Compute the Sec-WebSocket-Accept value from a client's Sec-WebSocket-Key.
///
/// The algorithm (RFC 6455 §1.3):
///   1. Concatenate the key with the WebSocket GUID
///   2. Compute SHA-1 hash
///   3. Base64 encode the result
///
/// @param key  The value of the Sec-WebSocket-Key header
/// @return     The computed Sec-WebSocket-Accept value
[[nodiscard]] UpgradeValidationResult::B64EncodedSha1 ComputeWebSocketAccept(std::string_view key);

/// Check if a Connection header value contains "upgrade" (case-insensitive).
///
/// The Connection header may contain multiple comma-separated tokens.
/// This function checks if any of them is "upgrade".
///
/// @param connectionValue  The value of the Connection header
/// @return                 True if "upgrade" token is present
[[nodiscard]] bool ConnectionContainsUpgrade(std::string_view connectionValue);

/// Validate the format of a Sec-WebSocket-Key.
///
/// A valid key is exactly 24 base64 characters (representing 16 random bytes).
///
/// @param key  The value of the Sec-WebSocket-Key header
/// @return     True if the key format is valid
[[nodiscard]] bool IsValidWebSocketKey(std::string_view key);

}  // namespace upgrade

}  // namespace aeronet
