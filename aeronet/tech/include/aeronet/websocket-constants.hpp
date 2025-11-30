#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aeronet::websocket {

// WebSocket Protocol Constants (RFC 6455)
// ========================================

// The magic GUID used in the Sec-WebSocket-Accept calculation (RFC 6455 §1.3)
inline constexpr std::string_view kWebSocketGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// WebSocket version we support
inline constexpr std::string_view kWebSocketVersion = "13";

// Header field names specific to WebSocket handshake
inline constexpr std::string_view SecWebSocketKey = "Sec-WebSocket-Key";
inline constexpr std::string_view SecWebSocketAccept = "Sec-WebSocket-Accept";
inline constexpr std::string_view SecWebSocketVersion = "Sec-WebSocket-Version";
inline constexpr std::string_view SecWebSocketProtocol = "Sec-WebSocket-Protocol";
inline constexpr std::string_view SecWebSocketExtensions = "Sec-WebSocket-Extensions";

// Expected value of Connection header for WebSocket upgrade
inline constexpr std::string_view UpgradeValue = "websocket";

// WebSocket Frame Opcodes (RFC 6455 §5.2)
// ========================================
// Opcodes are 4-bit values in the frame header that define the interpretation
// of the payload data.
enum class Opcode : uint8_t {
  // Data frames (0x0 - 0x7)
  Continuation = 0x0,  // Continuation frame for fragmented messages
  Text = 0x1,          // Text frame (payload is UTF-8 encoded)
  Binary = 0x2,        // Binary frame (payload is arbitrary binary data)
  // 0x3-0x7 reserved for future non-control frames

  // Control frames (0x8 - 0xF)
  Close = 0x8,  // Close frame (initiates connection close)
  Ping = 0x9,   // Ping frame (heartbeat request)
  Pong = 0xA,   // Pong frame (heartbeat response)
  // 0xB-0xF reserved for future control frames
};

// Check if an opcode is a control frame (opcodes 0x8-0xF)
[[nodiscard]] constexpr bool IsControlFrame(Opcode op) noexcept { return static_cast<uint8_t>(op) >= 0x8; }

// Check if an opcode is a data frame (opcodes 0x0-0x7)
[[nodiscard]] constexpr bool IsDataFrame(Opcode op) noexcept { return static_cast<uint8_t>(op) <= 0x7; }

// Check if an opcode is reserved/undefined
[[nodiscard]] constexpr bool IsReservedOpcode(std::byte rawOpcode) noexcept {
  // Reserved non-control: 0x3-0x7
  // Reserved control: 0xB-0xF
  return (rawOpcode >= std::byte{0x3} && rawOpcode <= std::byte{0x7}) ||
         (rawOpcode >= std::byte{0xB} && rawOpcode <= std::byte{0xF});
}

// WebSocket Close Status Codes (RFC 6455 §7.4.1)
// ==============================================
// Status codes indicate the reason for closing the connection.
// Codes 0-999 are not used.
// Codes 1000-2999 are defined by RFC 6455.
// Codes 3000-3999 are reserved for libraries/frameworks.
// Codes 4000-4999 are reserved for private use (application-defined).
enum class CloseCode : uint16_t {
  // Standard close codes (RFC 6455)
  Normal = 1000,              // Normal closure
  GoingAway = 1001,           // Endpoint is going away (e.g., server shutdown, browser navigating away)
  ProtocolError = 1002,       // Protocol error detected
  UnsupportedData = 1003,     // Received data type not supported (e.g., binary when only text expected)
  Reserved = 1004,            // Reserved (must not be used)
  NoStatusReceived = 1005,    // Reserved for use in APIs (no status code was present)
  AbnormalClosure = 1006,     // Reserved for use in APIs (connection closed abnormally)
  InvalidPayloadData = 1007,  // Received data inconsistent with message type (e.g., non-UTF-8 in text frame)
  PolicyViolation = 1008,     // Policy violation (generic)
  MessageTooBig = 1009,       // Message too big to process
  MandatoryExtension = 1010,  // Client expected extension negotiation that server didn't provide
  InternalError = 1011,       // Server encountered unexpected condition
  ServiceRestart = 1012,      // Server is restarting (RFC 6455 reserved, commonly used)
  TryAgainLater = 1013,       // Server overloaded, try again later (RFC 6455 reserved, commonly used)
  BadGateway = 1014,          // Gateway/proxy received invalid response from upstream
  TLSHandshake = 1015,        // Reserved for use in APIs (TLS handshake failure)
};

// Check if a close code is valid for transmission in a Close frame
// Codes 1005 and 1006 are reserved for APIs only (must not be sent over wire)
[[nodiscard]] constexpr bool IsValidWireCloseCode(uint16_t code) noexcept {
  return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1011) || (code >= 3000 && code <= 4999);
}

// WebSocket Frame Flags (RFC 6455 §5.2)
// =====================================
// First byte of frame: FIN | RSV1 | RSV2 | RSV3 | OPCODE (4 bits)
inline constexpr std::byte kFinBit{0x80};   // FIN: final fragment in message
inline constexpr std::byte kRsv1Bit{0x40};  // RSV1: reserved for extensions (e.g., per-message deflate)
inline constexpr std::byte kRsv2Bit{0x20};  // RSV2: reserved for extensions
inline constexpr std::byte kRsv3Bit{0x10};  // RSV3: reserved for extensions
inline constexpr std::byte kOpcodeMask{0x0F};

// Second byte: MASK | Payload length (7 bits)
inline constexpr std::byte kMaskBit{0x80};         // MASK: payload is masked (client -> server MUST be masked)
inline constexpr std::byte kPayloadLenMask{0x7F};  // Payload length (or indicator for extended length)

// Extended payload length indicators
inline constexpr std::byte kPayloadLen16{126};  // Actual length is in next 2 bytes (16-bit big-endian)
inline constexpr std::byte kPayloadLen64{127};  // Actual length is in next 8 bytes (64-bit big-endian)

// Maximum control frame payload size (RFC 6455 §5.5)
inline constexpr std::size_t kMaxControlFramePayload = 125;

// Masking key size in bytes
inline constexpr std::size_t kMaskingKeySize = 4;

// Minimum and maximum frame header sizes
inline constexpr std::size_t kMinFrameHeaderSize = 2;                              // No mask, 7-bit len
inline constexpr std::size_t kMaxFrameHeaderSize = 2 + 8 + kMaskingKeySize;        // 64-bit len + mask
inline constexpr std::size_t kMaxClientFrameHeaderSize = 2 + 8 + kMaskingKeySize;  // Client always masks
inline constexpr std::size_t kMaxServerFrameHeaderSize = 2 + 8;                    // Server never masks

// Default limits (can be overridden in configuration)
inline constexpr std::size_t kDefaultMaxMessageSize = 64UL * 1024UL * 1024UL;  // 64 MiB
inline constexpr std::size_t kDefaultMaxFrameSize = 16UL * 1024UL * 1024UL;    // 16 MiB

}  // namespace aeronet::websocket
