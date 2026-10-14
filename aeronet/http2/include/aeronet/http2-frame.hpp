#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-process-result-error-msg.hpp"
#include "aeronet/raw-bytes.hpp"

namespace aeronet::http2 {

/// HTTP/2 frame header (9 bytes) as defined in RFC 9113 §4.1.
/// Layout: Length (3 bytes) | Type (1 byte) | Flags (1 byte) | Reserved (1 bit) | Stream ID (31 bits)
/// The 9 bytes are stored in wire order in two integers and decoded on access: parsing and writing a header are then a
/// single 8-byte + 1-byte load / store, and as each half of the object is a single integer, compilers pass and return
/// it in two registers without assembling it in memory (GCC stores the fields of a struct one by one and reloads them
/// as a whole, a store forwarding stall).
class FrameHeader {
 public:
  static constexpr uint32_t kSize = 9;
  static constexpr uint32_t kMaxLength = (1U << 24U) - 1U;

  /// Build a frame header from its fields. length must fit in 24 bits, the reserved bit of streamId is ignored.
  constexpr FrameHeader(uint32_t length, FrameType type, uint8_t flags, uint32_t streamId) noexcept
      : _bytes0To7((static_cast<uint64_t>(length) << 40U) | (static_cast<uint64_t>(type) << 32U) |
                   (static_cast<uint64_t>(flags) << 24U) | ((streamId & kMaxWindowSize) >> 8U)),
        _byte8(static_cast<uint8_t>(streamId)) {
    assert(length <= kMaxLength && "frame length exceeds 24-bit protocol limit");
  }

  /// Payload length (24 bits).
  [[nodiscard]] constexpr uint32_t length() const noexcept { return static_cast<uint32_t>(_bytes0To7 >> 40U); }

  /// Frame type.
  [[nodiscard]] constexpr FrameType type() const noexcept { return static_cast<FrameType>(_bytes0To7 >> 32U); }

  /// Frame flags.
  [[nodiscard]] constexpr uint8_t flags() const noexcept { return static_cast<uint8_t>(_bytes0To7 >> 24U); }

  /// Stream identifier (31 bits, the reserved bit is never set).
  [[nodiscard]] constexpr uint32_t streamId() const noexcept {
    return (static_cast<uint32_t>(_bytes0To7) << 8U) | _byte8;
  }

  /// Check if a specific flag is set.
  [[nodiscard]] constexpr bool hasFlag(uint8_t flag) const noexcept { return (flags() & flag) != 0; }

  bool operator==(const FrameHeader&) const noexcept = default;

 private:
  friend FrameHeader ParseFrameHeader(std::span<const std::byte> data) noexcept;
  friend void WriteFrameHeader(std::byte* buffer, FrameHeader header) noexcept;

  // Reserved bit of the stream ID in _bytes0To7 (most significant bit of byte 5).
  static constexpr uint64_t kReservedBit = 1U << 23U;

  constexpr FrameHeader(uint64_t bytes0To7, uint8_t byte8) noexcept : _bytes0To7(bytes0To7), _byte8(byte8) {}

  uint64_t _bytes0To7{};  // bytes 0 to 7, as a big-endian integer, with the reserved bit cleared
  uint8_t _byte8{};       // byte 8 (low byte of the stream ID)
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
};

/// Parsed HEADERS frame (excluding the header block fragment which needs HPACK decoding).
/// The END_STREAM, END_HEADERS and PRIORITY flags are read from the frame header, the stream dependency is returned
/// apart by ParseHeadersFrame(). The priority fields are only set when the PRIORITY flag is.
struct HeadersFrame {
  /// The header block fragment, to be HPACK decoded.
  [[nodiscard]] std::span<const std::byte> headerBlockFragment() const noexcept {
    return {pHeaderBlockFragment, headerBlockFragmentSize};
  }

  const std::byte* pHeaderBlockFragment;
  uint32_t headerBlockFragmentSize;  // at most the 24-bit frame payload length
  uint8_t weightMinusOne;            // 0-255 (wire value)
  bool exclusive;
};

/// Parsed PRIORITY frame.
struct PriorityFrame {
  uint32_t streamDependency;
  uint8_t weightMinusOne;  // 0-255 (wire value)
  bool exclusive;
};

/// Parsed RST_STREAM frame.
struct RstStreamFrame {
  ErrorCode errorCode;
};

/// Parsed PING frame.
struct PingFrame {
  std::byte opaqueData[8];
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
enum class FrameParseResult : uint8_t {
  Ok,
  NeedMoreData,
  FrameSizeError,
  ProtocolError,
  InvalidPadding,
  InvalidDependency,
};

/// Parse a DATA frame payload.
[[nodiscard]] FrameParseResult ParseDataFrame(FrameHeader header, std::span<const std::byte> payload,
                                              DataFrame& out) noexcept;

/// Parse a HEADERS frame payload. streamDependency is only set when the PRIORITY flag is.
[[nodiscard]] FrameParseResult ParseHeadersFrame(FrameHeader header, std::span<const std::byte> payload,
                                                 HeadersFrame& out, uint32_t& streamDependency) noexcept;

/// Parse a PRIORITY frame payload.
[[nodiscard]] PriorityFrame ParsePriorityFrame(std::span<const std::byte> payload) noexcept;

/// Parse a RST_STREAM frame payload.
[[nodiscard]] FrameParseResult ParseRstStreamFrame(std::span<const std::byte> payload, RstStreamFrame& out) noexcept;

/// Parse a PING frame payload.
[[nodiscard]] FrameParseResult ParsePingFrame(FrameHeader header, std::span<const std::byte> payload,
                                              PingFrame& out) noexcept;

/// Parse a GOAWAY frame payload.
[[nodiscard]] FrameParseResult ParseGoAwayFrame(std::span<const std::byte> payload, GoAwayFrame& out) noexcept;

/// Parse a WINDOW_UPDATE frame payload.
[[nodiscard]] FrameParseResult ParseWindowUpdateFrame(std::span<const std::byte> payload,
                                                      WindowUpdateFrame& out) noexcept;

/// Parse a CONTINUATION frame payload.
void ParseContinuationFrame(FrameHeader header, std::span<const std::byte> payload, ContinuationFrame& out) noexcept;

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
std::byte* PrepareDataFrameGetStartPtr(RawBytes& buffer, uint32_t streamId, uint32_t dataSz, bool endStream);

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
std::size_t WritePingFrame(RawBytes& buffer, PingFrame pingFrame);

/// Write a GOAWAY frame.
std::size_t WriteGoAwayFrame(RawBytes& buffer, uint32_t lastStreamId, ErrorCode errorCode, ErrorMsg msg);

/// Write a WINDOW_UPDATE frame.
std::size_t WriteWindowUpdateFrame(RawBytes& buffer, uint32_t streamId, uint32_t windowSizeIncrement);

}  // namespace aeronet::http2
