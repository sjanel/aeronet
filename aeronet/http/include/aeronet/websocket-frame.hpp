#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/websocket-constants.hpp"

namespace aeronet::websocket {

/// 4-byte masking key type.
using MaskingKey = std::array<std::byte, kMaskingKeySize>;

/// Parsed WebSocket frame header.
/// Does not own the payload data - the payload is a view into the input buffer.
struct FrameHeader {
  /// Total header size in bytes (2-14 bytes depending on payload length and mask).
  [[nodiscard]] std::size_t headerSize() const noexcept;

  Opcode opcode{Opcode::Text};
  bool fin{true};      // FIN bit: true if this is the final fragment
  bool masked{false};  // MASK bit: true if payload is masked (client->server)
  bool rsv1{false};    // RSV1 bit (used by extensions like permessage-deflate)
  bool rsv2{false};    // RSV2 bit (reserved)
  bool rsv3{false};    // RSV3 bit (reserved)
  uint64_t payloadLength{0};
  std::array<std::byte, kMaskingKeySize> maskingKey{};
};

/// Result of parsing a WebSocket frame from raw bytes.
struct FrameParseResult {
  enum class Status : uint8_t {
    Complete,        // Frame fully parsed, header and payload available
    Incomplete,      // Need more data to parse the frame
    ProtocolError,   // Invalid frame format (connection should be closed with 1002)
    PayloadTooLarge  // Payload exceeds configured maximum (close with 1009)
  };

  Status status{Status::Incomplete};
  FrameHeader header;
  std::span<const std::byte> payload;  // View into input buffer (empty if incomplete)
  std::size_t bytesConsumed{0};        // Total bytes consumed (header + payload)
  std::string_view errorMessage;       // Populated on ProtocolError
};

/// Parse a WebSocket frame from raw bytes.
///
/// @param data           Input buffer containing raw WebSocket data
/// @param maxPayloadSize Maximum allowed payload size (0 = unlimited)
/// @param isServerSide   True if we're the server (clients MUST mask, servers MUST NOT)
/// @param allowRsv1      True if RSV1 bit is allowed (when permessage-deflate is negotiated)
/// @return               Parse result with frame data or error status
///
/// Thread safety: Not thread-safe (designed for single-threaded event loop).
[[nodiscard]] FrameParseResult ParseFrame(std::span<const std::byte> data, std::size_t maxPayloadSize = 0,
                                          bool isServerSide = true, bool allowRsv1 = false);

/// Apply XOR masking to WebSocket payload data.
///
/// The same function is used for both masking and unmasking (XOR is symmetric).
/// Modifies the data in place.
///
/// @param data       Payload data to mask/unmask (modified in place)
/// @param maskingKey 4-byte masking key from frame header
void ApplyMask(std::span<std::byte> data, std::span<const std::byte, kMaskingKeySize> maskingKey);

/// Build a WebSocket frame and append it to an output buffer.
///
/// @param output      Output buffer to append the frame to
/// @param opcode      Frame opcode (Text, Binary, Close, Ping, Pong)
/// @param payload     Payload data (empty allowed for control frames)
/// @param fin         FIN bit (true for complete messages, false for fragments)
/// @param mask        Whether to mask the payload (servers should NOT mask)
/// @param maskingKey  Masking key (only used if mask=true, random 4 bytes)
/// @param rsv1        RSV1 bit (true when payload is compressed with permessage-deflate)
///
/// Control frames (Close, Ping, Pong) must have payload <= 125 bytes and FIN=true.
void BuildFrame(RawBytes& output, Opcode opcode, std::span<const std::byte> payload, bool fin = true, bool mask = false,
                MaskingKey maskingKey = {}, bool rsv1 = false);

/// Convenience overload for text payloads.
inline void BuildFrame(RawBytes& output, Opcode opcode, std::string_view payload, bool fin = true, bool mask = false,
                       MaskingKey maskingKey = {}, bool rsv1 = false) {
  BuildFrame(output, opcode, std::as_bytes(std::span(payload)), fin, mask, maskingKey, rsv1);
}

/// Build a Close frame with an optional status code and reason.
///
/// @param output     Output buffer to append the frame to
/// @param code       Close status code (0 = no code)
/// @param reason     Optional close reason (UTF-8 string, max ~123 bytes)
/// @param mask       Whether to mask the payload
/// @param maskingKey Masking key (only used if mask=true)
void BuildCloseFrame(RawBytes& output, CloseCode code = CloseCode::Normal, std::string_view reason = {},
                     bool mask = false, MaskingKey maskingKey = {});

/// Parse a Close frame payload to extract status code and reason.
///
/// @param payload Close frame payload (2+ bytes for code, remainder is reason)
/// @return        Pair of (status code, reason). Code is 0 if payload is empty.
struct ClosePayload {
  CloseCode code{CloseCode::NoStatusReceived};
  std::string_view reason;
};

[[nodiscard]] ClosePayload ParseClosePayload(std::span<const std::byte> payload);

}  // namespace aeronet::websocket
