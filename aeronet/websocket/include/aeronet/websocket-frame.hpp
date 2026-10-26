#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/websocket-constants.hpp"

namespace aeronet::websocket {

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
  MaskingKey maskingKey{};
};

/// Result of parsing a WebSocket frame from raw bytes.
struct FrameParseResult {
  enum class Status : uint8_t {
    Complete,         // Frame fully parsed, header and payload available
    Incomplete,       // Need more data to parse the frame
    ProtocolError,    // Invalid frame format (connection should be closed with 1002)
    PayloadTooLarge,  // Payload exceeds configured maximum (close with 1009)
  };

  Status status{Status::Incomplete};
  FrameHeader header;
  std::span<const std::byte> payload;  // View into input buffer (empty if incomplete)
  std::size_t bytesConsumed{0};        // Total bytes consumed (header + payload)
  const char* pErrorMessage{nullptr};  // Populated on ProtocolError
};

/// Parse a WebSocket frame from raw bytes.
///
/// @param data           Input buffer containing raw WebSocket data
/// @param maxPayloadSize Maximum allowed payload size
/// @param isServerSide   True if we're the server (clients MUST mask, servers MUST NOT)
/// @param allowRsv1      True if RSV1 bit is allowed (when permessage-deflate is negotiated)
/// @return               Parse result with frame data or error status
///
/// Thread safety: Not thread-safe (designed for single-threaded event loop).
[[nodiscard]] FrameParseResult ParseFrame(std::span<const std::byte> data,
                                          std::size_t maxPayloadSize = std::numeric_limits<std::size_t>::max(),
                                          bool isServerSide = true, bool allowRsv1 = false);

/// Apply XOR masking to WebSocket payload data.
///
/// The same function is used for both masking and unmasking (XOR is symmetric).
/// Modifies the data in place.
///
/// @param data       Payload data to mask/unmask (modified in place), from the start of the frame payload
/// @param maskingKey 4-byte masking key from frame header
void ApplyMask(std::span<std::byte> data, MaskingKey maskingKey);

/// Copy src to dst, masked (or unmasked) with maskingKey, in a single pass: same result as a copy followed by
/// ApplyMask() on dst. src starts at the start of a frame payload, dst must not overlap it.
/// @return dst + src.size()
std::byte* AppendMasked(std::span<const std::byte> src, std::byte* dst, MaskingKey maskingKey);

/// Build a WebSocket frame and append it to an output buffer.
///
/// @param output      Output buffer to append the frame to
/// @param opcode      Frame opcode (Text, Binary, Close, Ping, Pong)
/// @param payload     Payload data (empty allowed for control frames)
/// @param fin         FIN bit (true for complete messages, false for fragments)
/// @param shouldMask  Whether to mask the payload (servers should NOT mask)
/// @param maskingKey  Masking key (only used if shouldMask=true, random 4 bytes)
/// @param rsv1        RSV1 bit (true when payload is compressed with permessage-deflate)
///
/// Control frames (Close, Ping, Pong) must have payload <= 125 bytes and FIN=true.
void BuildFrame(RawBytes& output, Opcode opcode, std::span<const std::byte> payload, bool fin = true,
                bool shouldMask = false, MaskingKey maskingKey = {}, bool rsv1 = false);

/// Append only the header of a frame of payloadSize bytes (FIN set) to an output buffer: the payload is sent from
/// where it is, right after the header, already masked with maskingKey if shouldMask.
void BuildFrameHeader(RawBytes& output, Opcode opcode, std::size_t payloadSize, bool shouldMask = false,
                      MaskingKey maskingKey = {}, bool rsv1 = false);

/// Build a Close frame with an optional status code and reason.
///
/// @param output     Output buffer to append the frame to
/// @param code       Close status code (0 = no code)
/// @param reason     Optional close reason (UTF-8 string, max ~123 bytes)
/// @param shouldMask  Whether to mask the payload
/// @param maskingKey  Masking key (only used if shouldMask=true)
void BuildCloseFrame(RawBytes& output, CloseCode code = CloseCode::Normal, std::string_view reason = {},
                     bool shouldMask = false, MaskingKey maskingKey = {});

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
