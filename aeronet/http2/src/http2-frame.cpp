#include "aeronet/http2-frame.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "aeronet/http2-frame-types.hpp"
#include "aeronet/raw-bytes.hpp"

namespace aeronet::http2 {

namespace {

// Read a 24-bit big-endian value.
constexpr uint32_t Read24BE(const std::byte* data) {
  return (static_cast<uint32_t>(data[0]) << 16) | (static_cast<uint32_t>(data[1]) << 8) |
         static_cast<uint32_t>(data[2]);
}

// Read a 32-bit big-endian value.
constexpr uint32_t Read32BE(const std::byte* data) noexcept {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

// Write a 24-bit big-endian value.
constexpr void Write24BE(std::byte* data, uint32_t value) noexcept {
  data[0] = static_cast<std::byte>((value >> 16) & 0xFF);
  data[1] = static_cast<std::byte>((value >> 8) & 0xFF);
  data[2] = static_cast<std::byte>(value & 0xFF);
}

// Write a 32-bit big-endian value.
constexpr void Write32BE(std::byte* data, uint32_t value) noexcept {
  data[0] = static_cast<std::byte>((value >> 24) & 0xFF);
  data[1] = static_cast<std::byte>((value >> 16) & 0xFF);
  data[2] = static_cast<std::byte>((value >> 8) & 0xFF);
  data[3] = static_cast<std::byte>(value & 0xFF);
}

// Write a 16-bit big-endian value.
constexpr void Write16BE(std::byte* data, uint16_t value) noexcept {
  data[0] = static_cast<std::byte>((value >> 8) & 0xFF);
  data[1] = static_cast<std::byte>(value & 0xFF);
}

// Read a 16-bit big-endian value.
constexpr uint16_t Read16BE(const std::byte* data) noexcept {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]));
}

constexpr FrameHeader CreateFrameHeader(FrameType type, uint8_t flags, uint32_t streamId,
                                        uint32_t payloadLength) noexcept {
  FrameHeader header;
  header.type = type;
  header.flags = flags;
  header.streamId = streamId;
  header.length = payloadLength;
  return header;
}

}  // namespace

// ============================
// Frame header parsing/writing
// ============================

FrameHeader ParseFrameHeader(std::span<const std::byte> data) noexcept {
  assert(data.size() >= FrameHeader::kSize);

  return CreateFrameHeader(static_cast<FrameType>(static_cast<uint8_t>(data[3])), static_cast<uint8_t>(data[4]),
                           Read32BE(data.data() + 5) & 0x7FFFFFFF, Read24BE(data.data()));
}

void WriteFrameHeader(std::byte* buffer, FrameHeader header) noexcept {
  Write24BE(buffer, header.length);
  buffer[3] = static_cast<std::byte>(header.type);
  buffer[4] = static_cast<std::byte>(header.flags);
  Write32BE(buffer + 5, header.streamId & 0x7FFFFFFF);  // Clear reserved bit
}

std::size_t WriteFrame(RawBytes& buffer, FrameType type, uint8_t flags, uint32_t streamId, uint32_t payloadSize) {
  const std::size_t totalSize = FrameHeader::kSize + payloadSize;

  buffer.ensureAvailableCapacityExponential(totalSize);

  WriteFrameHeader(buffer.data() + buffer.size(), CreateFrameHeader(type, flags, streamId, payloadSize));
  buffer.addSize(FrameHeader::kSize);

  return totalSize;
}

// ============================
// Frame parsing functions
// ============================

FrameParseResult ParseDataFrame(FrameHeader header, std::span<const std::byte> payload, DataFrame& out) noexcept {
  out.endStream = header.hasFlag(FrameFlags::DataEndStream);
  out.padLength = 0;

  if (header.hasFlag(FrameFlags::DataPadded)) {
    if (payload.empty()) {
      return FrameParseResult::FrameSizeError;
    }
    out.padLength = static_cast<uint8_t>(payload[0]);
    if (out.padLength >= payload.size()) {
      return FrameParseResult::InvalidPadding;
    }
    out.data = payload.subspan(1, payload.size() - 1 - out.padLength);
  } else {
    out.data = payload;
  }

  return FrameParseResult::Ok;
}

FrameParseResult ParseHeadersFrame(FrameHeader header, std::span<const std::byte> payload, HeadersFrame& out) noexcept {
  out.endStream = header.hasFlag(FrameFlags::HeadersEndStream);
  out.endHeaders = header.hasFlag(FrameFlags::HeadersEndHeaders);
  out.hasPriority = header.hasFlag(FrameFlags::HeadersPriority);
  out.padLength = 0;
  out.exclusive = false;
  out.streamDependency = 0;
  out.weight = 16;

  std::size_t offset = 0;

  // Handle padding
  if (header.hasFlag(FrameFlags::HeadersPadded)) {
    if (payload.empty()) {
      return FrameParseResult::FrameSizeError;
    }
    out.padLength = static_cast<uint8_t>(payload[0]);
    offset = 1;
  }

  // Handle priority
  if (out.hasPriority) {
    if (payload.size() < offset + 5) {
      return FrameParseResult::FrameSizeError;
    }
    uint32_t depAndExcl = Read32BE(payload.data() + offset);
    out.exclusive = (depAndExcl & 0x80000000) != 0;
    out.streamDependency = depAndExcl & 0x7FFFFFFF;
    // RFC 9113 §5.3.1: "Add one to the value to obtain a weight between 1 and 256."
    out.weight = static_cast<uint8_t>(payload[offset + 4]) + 1;
    offset += 5;
  }

  // Validate padding
  if (out.padLength > 0 && offset + out.padLength > payload.size()) {
    return FrameParseResult::InvalidPadding;
  }

  out.headerBlockFragment = payload.subspan(offset, payload.size() - offset - out.padLength);
  return FrameParseResult::Ok;
}

FrameParseResult ParsePriorityFrame(FrameHeader /*header*/, std::span<const std::byte> payload,
                                    PriorityFrame& out) noexcept {
  if (payload.size() != 5) {
    return FrameParseResult::FrameSizeError;
  }

  uint32_t depAndExcl = Read32BE(payload.data());
  out.exclusive = (depAndExcl & 0x80000000) != 0;
  out.streamDependency = depAndExcl & 0x7FFFFFFF;
  // RFC 9113 §5.3.1: "Add one to the value to obtain a weight between 1 and 256."
  out.weight = static_cast<uint8_t>(payload[4]) + 1;

  return FrameParseResult::Ok;
}

FrameParseResult ParseRstStreamFrame(FrameHeader /*header*/, std::span<const std::byte> payload,
                                     RstStreamFrame& out) noexcept {
  if (payload.size() != 4) {
    return FrameParseResult::FrameSizeError;
  }

  out.errorCode = static_cast<ErrorCode>(Read32BE(payload.data()));
  return FrameParseResult::Ok;
}

FrameParseResult ParseSettingsFrame(FrameHeader header, std::span<const std::byte> payload,
                                    SettingsFrame& out) noexcept {
  out.isAck = header.hasFlag(FrameFlags::SettingsAck);
  out.entryCount = 0;

  if (out.isAck) {
    if (!payload.empty()) {
      return FrameParseResult::FrameSizeError;
    }
    return FrameParseResult::Ok;
  }

  if (payload.size() % 6 != 0) {
    return FrameParseResult::FrameSizeError;
  }

  std::size_t numEntries = payload.size() / 6;
  numEntries = std::min(numEntries, SettingsFrame::kMaxEntries);

  for (std::size_t idx = 0; idx < numEntries; ++idx) {
    const std::byte* entry = payload.data() + (idx * 6);
    out.entries[idx].id = static_cast<SettingsParameter>(Read16BE(entry));
    out.entries[idx].value = Read32BE(entry + 2);
  }
  out.entryCount = numEntries;

  return FrameParseResult::Ok;
}

FrameParseResult ParsePingFrame(FrameHeader header, std::span<const std::byte> payload, PingFrame& out) noexcept {
  if (payload.size() != 8) {
    return FrameParseResult::FrameSizeError;
  }

  out.isAck = header.hasFlag(FrameFlags::PingAck);
  std::memcpy(out.opaqueData.data(), payload.data(), 8);

  return FrameParseResult::Ok;
}

FrameParseResult ParseGoAwayFrame(FrameHeader /*header*/, std::span<const std::byte> payload,
                                  GoAwayFrame& out) noexcept {
  if (payload.size() < 8) {
    return FrameParseResult::FrameSizeError;
  }

  out.lastStreamId = Read32BE(payload.data()) & 0x7FFFFFFF;
  out.errorCode = static_cast<ErrorCode>(Read32BE(payload.data() + 4));
  out.debugData = payload.subspan(8);

  return FrameParseResult::Ok;
}

FrameParseResult ParseWindowUpdateFrame(std::span<const std::byte> payload, WindowUpdateFrame& out) noexcept {
  if (payload.size() != 4) {
    return FrameParseResult::FrameSizeError;
  }

  out.windowSizeIncrement = Read32BE(payload.data()) & 0x7FFFFFFF;  // Clear reserved bit
  return FrameParseResult::Ok;
}

FrameParseResult ParseContinuationFrame(FrameHeader header, std::span<const std::byte> payload,
                                        ContinuationFrame& out) noexcept {
  out.endHeaders = header.hasFlag(FrameFlags::ContinuationEndHeaders);
  out.headerBlockFragment = payload;
  return FrameParseResult::Ok;
}

// ============================
// Frame writing functions
// ============================

std::size_t WriteDataFrame(RawBytes& buffer, uint32_t streamId, std::span<const std::byte> data, bool endStream) {
  uint8_t flags = endStream ? FrameFlags::DataEndStream : FrameFlags::None;
  const auto ret = WriteFrame(buffer, FrameType::Data, flags, streamId, static_cast<uint32_t>(data.size()));
  buffer.unchecked_append(data);
  return ret;
}

std::size_t WriteHeadersFrameWithPriority(RawBytes& buffer, uint32_t streamId, std::span<const std::byte> headerBlock,
                                          uint32_t streamDependency, uint8_t weight, bool exclusive, bool endStream,
                                          bool endHeaders) {
  const std::size_t payloadSize = 5 + headerBlock.size();  // Priority is 5 bytes

  buffer.ensureAvailableCapacityExponential(FrameHeader::kSize + 5UL + headerBlock.size());

  WriteFrameHeader(
      buffer.end(),
      CreateFrameHeader(FrameType::Headers, ComputeHeaderFrameFlags(endStream, endHeaders, FrameFlags::HeadersPriority),
                        streamId, static_cast<uint32_t>(payloadSize)));
  buffer.addSize(FrameHeader::kSize);

  // Write priority
  uint32_t depWithExcl = streamDependency;
  if (exclusive) {
    depWithExcl |= 0x80000000;
  }
  Write32BE(buffer.end(), depWithExcl);
  buffer.addSize(4);
  buffer.unchecked_push_back(static_cast<std::byte>(weight));
  buffer.unchecked_append(headerBlock);

  return FrameHeader::kSize + payloadSize;
}

std::size_t WritePriorityFrame(RawBytes& buffer, uint32_t streamId, uint32_t streamDependency, uint8_t weight,
                               bool exclusive) {
  const auto ret = WriteFrame(buffer, FrameType::Priority, FrameFlags::None, streamId, 5U);

  uint32_t depWithExcl = streamDependency;
  if (exclusive) {
    depWithExcl |= 0x80000000;
  }
  Write32BE(buffer.end(), depWithExcl);
  buffer.end()[4] = static_cast<std::byte>(weight);

  buffer.addSize(5);
  return ret;
}

std::size_t WriteRstStreamFrame(RawBytes& buffer, uint32_t streamId, ErrorCode errorCode) {
  const auto ret = WriteFrame(buffer, FrameType::RstStream, FrameFlags::None, streamId, 4U);
  Write32BE(buffer.end(), static_cast<uint32_t>(errorCode));
  buffer.addSize(4);
  return ret;
}

std::size_t WriteSettingsFrame(RawBytes& buffer, std::span<const SettingsEntry> entries) {
  const std::size_t payloadSize = entries.size() * 6;

  buffer.ensureAvailableCapacityExponential(FrameHeader::kSize + (entries.size() * 6));

  WriteFrameHeader(buffer.end(),
                   CreateFrameHeader(FrameType::Settings, FrameFlags::None, 0, static_cast<uint32_t>(payloadSize)));
  buffer.addSize(FrameHeader::kSize);

  // Write settings entries
  for (const auto& entry : entries) {
    Write16BE(buffer.end(), static_cast<uint16_t>(entry.id));
    Write32BE(buffer.end() + 2, entry.value);
    buffer.addSize(6);
  }

  return FrameHeader::kSize + payloadSize;
}

std::size_t WriteSettingsAckFrame(RawBytes& buffer) {
  return WriteFrame(buffer, FrameType::Settings, FrameFlags::SettingsAck, 0, 0);
}

std::size_t WritePingFrame(RawBytes& buffer, std::span<const std::byte, 8> opaqueData, bool isAck) {
  uint8_t flags = isAck ? FrameFlags::PingAck : FrameFlags::None;
  const auto ret = WriteFrame(buffer, FrameType::Ping, flags, 0, opaqueData.size());
  buffer.unchecked_append(opaqueData);
  return ret;
}

std::size_t WriteGoAwayFrame(RawBytes& buffer, uint32_t lastStreamId, ErrorCode errorCode, std::string_view debugData) {
  const std::size_t payloadSize = 8 + debugData.size();

  buffer.ensureAvailableCapacityExponential(FrameHeader::kSize + 8UL + debugData.size());

  WriteFrameHeader(buffer.end(),
                   CreateFrameHeader(FrameType::GoAway, FrameFlags::None, 0, static_cast<uint32_t>(payloadSize)));
  buffer.addSize(FrameHeader::kSize);

  // Write last stream ID and error code
  Write32BE(buffer.end(), lastStreamId);
  Write32BE(buffer.end() + 4, static_cast<uint32_t>(errorCode));
  buffer.addSize(8);

  // Write debug data
  buffer.unchecked_append(reinterpret_cast<const std::byte*>(debugData.data()), debugData.size());

  return FrameHeader::kSize + payloadSize;
}

std::size_t WriteWindowUpdateFrame(RawBytes& buffer, uint32_t streamId, uint32_t windowSizeIncrement) {
  const auto ret = WriteFrame(buffer, FrameType::WindowUpdate, FrameFlags::None, streamId, 4U);
  Write32BE(buffer.end(), windowSizeIncrement & 0x7FFFFFFF);  // Clear reserved bit
  buffer.addSize(4);
  return ret;
}

std::size_t WriteContinuationFrame(RawBytes& buffer, uint32_t streamId, std::span<const std::byte> headerBlock,
                                   bool endHeaders) {
  uint8_t flags = endHeaders ? FrameFlags::ContinuationEndHeaders : FrameFlags::None;
  const auto ret =
      WriteFrame(buffer, FrameType::Continuation, flags, streamId, static_cast<uint32_t>(headerBlock.size()));
  buffer.unchecked_append(headerBlock);
  return ret;
}

}  // namespace aeronet::http2
