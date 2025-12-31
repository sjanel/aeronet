#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/http2-frame-types.hpp"
#include "aeronet/raw-bytes.hpp"

namespace aeronet::http2 {

/// Convert FrameType to human-readable string for logging/debugging.
constexpr std::string_view FrameTypeName(FrameType type) noexcept {
  switch (type) {
    case FrameType::Data:
      return "DATA";
    case FrameType::Headers:
      return "HEADERS";
    case FrameType::Priority:
      return "PRIORITY";
    case FrameType::RstStream:
      return "RST_STREAM";
    case FrameType::Settings:
      return "SETTINGS";
    case FrameType::PushPromise:
      return "PUSH_PROMISE";
    case FrameType::Ping:
      return "PING";
    case FrameType::GoAway:
      return "GOAWAY";
    case FrameType::WindowUpdate:
      return "WINDOW_UPDATE";
    case FrameType::Continuation:
      return "CONTINUATION";
    default:
      return "UNKNOWN";
  }
}

/// Convert ErrorCode to human-readable string for logging/debugging.
constexpr std::string_view ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::NoError:
      return "NO_ERROR";
    case ErrorCode::ProtocolError:
      return "PROTOCOL_ERROR";
    case ErrorCode::InternalError:
      return "INTERNAL_ERROR";
    case ErrorCode::FlowControlError:
      return "FLOW_CONTROL_ERROR";
    case ErrorCode::SettingsTimeout:
      return "SETTINGS_TIMEOUT";
    case ErrorCode::StreamClosed:
      return "STREAM_CLOSED";
    case ErrorCode::FrameSizeError:
      return "FRAME_SIZE_ERROR";
    case ErrorCode::RefusedStream:
      return "REFUSED_STREAM";
    case ErrorCode::Cancel:
      return "CANCEL";
    case ErrorCode::CompressionError:
      return "COMPRESSION_ERROR";
    case ErrorCode::ConnectError:
      return "CONNECT_ERROR";
    case ErrorCode::EnhanceYourCalm:
      return "ENHANCE_YOUR_CALM";
    case ErrorCode::InadequateSecurity:
      return "INADEQUATE_SECURITY";
    case ErrorCode::Http11Required:
      return "HTTP_1_1_REQUIRED";
    default:
      return "UNKNOWN_ERROR";
  }
}

/// HTTP/2 frame header (9 bytes) as defined in RFC 9113 §4.1.
/// Layout: Length (3 bytes) | Type (1 byte) | Flags (1 byte) | Reserved (1 bit) | Stream ID (31 bits)
struct FrameHeader {
  static constexpr std::size_t kSize = kFrameHeaderSize;

  /// Check if a specific flag is set.
  [[nodiscard]] constexpr bool hasFlag(uint8_t flag) const noexcept { return (flags & flag) != 0; }

  /// Check if this is a valid frame header (basic sanity checks).
  [[nodiscard]] constexpr bool isValid() const noexcept {
    // Length must fit in 24 bits
    return length <= 0x00FFFFFF;
  }

  uint32_t length;  ///< Payload length (24 bits, max 16777215)
  FrameType type;
  uint8_t flags;
  uint32_t streamId;  ///< 31-bit stream identifier
};

/// Parse a 9-byte frame header from raw bytes.
/// Returns a FrameHeader with parsed values.
/// Precondition: data.size() >= FrameHeader::kSize
[[nodiscard]] FrameHeader ParseFrameHeader(std::span<const std::byte> data) noexcept;

/// Serialize a frame header to a 9-byte buffer.
/// Precondition: buffer.size() >= FrameHeader::kSize
void WriteFrameHeader(std::byte* buffer, FrameHeader header) noexcept;

/// Helper to write a complete frame (header + payload) to a buffer.
/// Returns the total number of bytes written.
/// The buffer must have sufficient capacity (header + payload).
std::size_t WriteFrame(RawBytes& buffer, FrameType type, uint8_t flags, uint32_t streamId, uint32_t payloadSize);

// ============================
// Frame-specific structures
// ============================

/// SETTINGS frame parameter (identifier + value pair).
struct SettingsEntry {
  SettingsParameter id;
  uint32_t value;
};

/// Parsed DATA frame.
struct DataFrame {
  std::span<const std::byte> data;
  uint8_t padLength;
  bool endStream;
};

/// Parsed HEADERS frame (excluding the header block fragment which needs HPACK decoding).
struct HeadersFrame {
  std::span<const std::byte> headerBlockFragment;
  uint32_t streamDependency;
  uint16_t weight;  // 1-256 (wire value + 1)
  uint8_t padLength;
  bool endStream;
  bool endHeaders;
  bool exclusive;
  bool hasPriority;
};

/// Parsed PRIORITY frame.
struct PriorityFrame {
  uint32_t streamDependency;
  uint16_t weight;  // 1-256 (wire value + 1)
  bool exclusive;
};

/// Parsed RST_STREAM frame.
struct RstStreamFrame {
  ErrorCode errorCode;
};

/// Parsed SETTINGS frame.
struct SettingsFrame {
  static constexpr std::size_t kMaxEntries = 6;  // RFC defines 6 standard settings

  SettingsEntry entries[kMaxEntries];
  std::size_t entryCount;
  bool isAck;
};

/// Parsed PING frame.
struct PingFrame {
  std::array<std::byte, 8> opaqueData;
  bool isAck;
};

/// Parsed GOAWAY frame.
struct GoAwayFrame {
  uint32_t lastStreamId;
  ErrorCode errorCode;
  std::span<const std::byte> debugData;
};

/// Parsed WINDOW_UPDATE frame.
struct WindowUpdateFrame {
  uint32_t windowSizeIncrement;
};

/// Parsed CONTINUATION frame.
struct ContinuationFrame {
  std::span<const std::byte> headerBlockFragment;
  bool endHeaders;
};

// ============================
// Frame parsing functions
// ============================

/// Parse result for frame parsing operations.
enum class FrameParseResult : uint8_t { Ok, NeedMoreData, FrameSizeError, ProtocolError, InvalidPadding };

/// Parse a DATA frame payload.
[[nodiscard]] FrameParseResult ParseDataFrame(FrameHeader header, std::span<const std::byte> payload,
                                              DataFrame& out) noexcept;

/// Parse a HEADERS frame payload.
[[nodiscard]] FrameParseResult ParseHeadersFrame(FrameHeader header, std::span<const std::byte> payload,
                                                 HeadersFrame& out) noexcept;

/// Parse a PRIORITY frame payload.
[[nodiscard]] FrameParseResult ParsePriorityFrame(FrameHeader header, std::span<const std::byte> payload,
                                                  PriorityFrame& out) noexcept;

/// Parse a RST_STREAM frame payload.
[[nodiscard]] FrameParseResult ParseRstStreamFrame(FrameHeader header, std::span<const std::byte> payload,
                                                   RstStreamFrame& out) noexcept;

/// Parse a SETTINGS frame payload.
[[nodiscard]] FrameParseResult ParseSettingsFrame(FrameHeader header, std::span<const std::byte> payload,
                                                  SettingsFrame& out) noexcept;

/// Parse a PING frame payload.
[[nodiscard]] FrameParseResult ParsePingFrame(FrameHeader header, std::span<const std::byte> payload,
                                              PingFrame& out) noexcept;

/// Parse a GOAWAY frame payload.
[[nodiscard]] FrameParseResult ParseGoAwayFrame(FrameHeader header, std::span<const std::byte> payload,
                                                GoAwayFrame& out) noexcept;

/// Parse a WINDOW_UPDATE frame payload.
[[nodiscard]] FrameParseResult ParseWindowUpdateFrame(std::span<const std::byte> payload,
                                                      WindowUpdateFrame& out) noexcept;

/// Parse a CONTINUATION frame payload.
[[nodiscard]] FrameParseResult ParseContinuationFrame(FrameHeader header, std::span<const std::byte> payload,
                                                      ContinuationFrame& out) noexcept;

// ============================
// Frame writing functions
// ============================

constexpr uint8_t ComputeHeaderFrameFlags(bool endStream, bool endHeaders, uint8_t init = FrameFlags::None) noexcept {
  if (endStream) {
    init |= FrameFlags::HeadersEndStream;
  }
  if (endHeaders) {
    init |= FrameFlags::HeadersEndHeaders;
  }
  return init;
}

/// Write a DATA frame.
std::size_t WriteDataFrame(RawBytes& buffer, uint32_t streamId, std::span<const std::byte> data, bool endStream);

/// Write a HEADERS frame with priority.
std::size_t WriteHeadersFrameWithPriority(RawBytes& buffer, uint32_t streamId, std::span<const std::byte> headerBlock,
                                          uint32_t streamDependency, uint8_t weight, bool exclusive, bool endStream,
                                          bool endHeaders);

/// Write a PRIORITY frame.
std::size_t WritePriorityFrame(RawBytes& buffer, uint32_t streamId, uint32_t streamDependency, uint8_t weight,
                               bool exclusive);

/// Write a RST_STREAM frame.
std::size_t WriteRstStreamFrame(RawBytes& buffer, uint32_t streamId, ErrorCode errorCode);

/// Write a SETTINGS frame.
std::size_t WriteSettingsFrame(RawBytes& buffer, std::span<const SettingsEntry> entries);

/// Write a SETTINGS ACK frame.
std::size_t WriteSettingsAckFrame(RawBytes& buffer);

/// Write a PING frame.
std::size_t WritePingFrame(RawBytes& buffer, std::span<const std::byte, 8> opaqueData, bool isAck);

/// Write a GOAWAY frame.
std::size_t WriteGoAwayFrame(RawBytes& buffer, uint32_t lastStreamId, ErrorCode errorCode,
                             std::string_view debugData = {});

/// Write a WINDOW_UPDATE frame.
std::size_t WriteWindowUpdateFrame(RawBytes& buffer, uint32_t streamId, uint32_t windowSizeIncrement);

/// Write a CONTINUATION frame.
std::size_t WriteContinuationFrame(RawBytes& buffer, uint32_t streamId, std::span<const std::byte> headerBlock,
                                   bool endHeaders);

}  // namespace aeronet::http2
