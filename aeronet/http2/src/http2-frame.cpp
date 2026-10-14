#include "aeronet/http2-frame.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-process-result-error-msg-strings.hpp"
#include "aeronet/http2-process-result-error-msg.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/raw-bytes.hpp"
#include "http2-read-write.hpp"

namespace aeronet::http2 {

// ============================
// Frame header parsing/writing
// ============================

FrameHeader ParseFrameHeader(std::span<const std::byte> data) noexcept {
  assert(data.size() >= FrameHeader::kSize);

  return {Read64BE(data.data()) & ~FrameHeader::kReservedBit, static_cast<uint8_t>(data[8])};
}

void WriteFrameHeader(std::byte* buffer, FrameHeader header) noexcept {
  Write64BE(buffer, header._bytes0To7);
  buffer[8] = static_cast<std::byte>(header._byte8);
}

std::size_t WriteFrame(RawBytes& buffer, FrameType type, uint8_t flags, uint32_t streamId, uint32_t payloadSize) {
  const std::size_t totalSize = FrameHeader::kSize + payloadSize;

  buffer.ensureAvailableCapacityExponential(totalSize);

  WriteFrameHeader(buffer.data() + buffer.size(), FrameHeader(payloadSize, type, flags, streamId));
  buffer.addSize(FrameHeader::kSize);

  return totalSize;
}

// ============================
// Frame parsing functions
// ============================

FrameParseResult ParseDataFrame(FrameHeader header, std::span<const std::byte> payload, DataFrame& out) noexcept {
  if (header.hasFlag(FrameFlags::DataPadded)) {
    if (payload.empty()) {
      return FrameParseResult::FrameSizeError;
    }
    const uint8_t padLength = static_cast<uint8_t>(payload[0]);
    if (padLength >= payload.size()) {
      return FrameParseResult::InvalidPadding;
    }
    out.data = payload.subspan(1, payload.size() - 1 - padLength);
  } else {
    out.data = payload;
  }

  return FrameParseResult::Ok;
}

FrameParseResult ParseHeadersFrame(FrameHeader header, std::span<const std::byte> payload, HeadersFrame& out,
                                   uint32_t& streamDependency) noexcept {
  uint8_t offset = 0;
  uint8_t padLength = 0;

  // Handle padding
  if (header.hasFlag(FrameFlags::HeadersPadded)) {
    if (payload.empty()) {
      return FrameParseResult::FrameSizeError;
    }
    padLength = static_cast<uint8_t>(payload[0]);
    offset = 1;
  }

  // Handle priority
  if (header.hasFlag(FrameFlags::HeadersPriority)) {
    if (payload.size() < offset + 5U) {
      return FrameParseResult::FrameSizeError;
    }

    PriorityFrame frame = ParsePriorityFrame(payload.subspan(offset, 5U));
    if (header.streamId() == frame.streamDependency) {
      return FrameParseResult::InvalidDependency;
    }
    out.exclusive = frame.exclusive;
    streamDependency = frame.streamDependency;
    out.weightMinusOne = frame.weightMinusOne;
    offset += 5U;
  }

  // Validate padding
  if (offset + padLength > payload.size()) {
    return FrameParseResult::InvalidPadding;
  }

  out.pHeaderBlockFragment = payload.data() + offset;
  out.headerBlockFragmentSize = static_cast<uint32_t>(payload.size() - offset - padLength);
  return FrameParseResult::Ok;
}

PriorityFrame ParsePriorityFrame(std::span<const std::byte> payload) noexcept {
  PriorityFrame frame;
  uint32_t depAndExcl = Read32BE(payload.data());
  frame.exclusive = (depAndExcl & 0x80000000U) != 0;
  frame.streamDependency = depAndExcl & kMaxWindowSize;
  // RFC 9113 §5.3.1: "Add one to the value to obtain a weight between 1 and 256."
  frame.weightMinusOne = static_cast<uint8_t>(payload[sizeof(uint32_t)]);

  return frame;
}

FrameParseResult ParseRstStreamFrame(std::span<const std::byte> payload, RstStreamFrame& out) noexcept {
  if (payload.size() != sizeof(uint32_t)) {
    return FrameParseResult::FrameSizeError;
  }

  static_assert(sizeof(ErrorCode) == sizeof(uint32_t), "ErrorCode must be 4 bytes (or logic below needs to change)");

  out.errorCode = static_cast<ErrorCode>(Read32BE(payload.data()));
  return FrameParseResult::Ok;
}

FrameParseResult ParsePingFrame(FrameHeader header, std::span<const std::byte> payload, PingFrame& out) noexcept {
  if (payload.size() != 8U) {
    return FrameParseResult::FrameSizeError;
  }

  out.isAck = header.hasFlag(FrameFlags::PingAck);
  std::memcpy(out.opaqueData, payload.data(), 8U);

  return FrameParseResult::Ok;
}

FrameParseResult ParseGoAwayFrame(std::span<const std::byte> payload, GoAwayFrame& out) noexcept {
  if (payload.size() < 2 * sizeof(uint32_t)) {
    return FrameParseResult::FrameSizeError;
  }

  static_assert(sizeof(ErrorCode) == sizeof(uint32_t), "ErrorCode must be 4 bytes (or logic below needs to change)");

  out.lastStreamId = Read32BE(payload.data()) & kMaxWindowSize;
  out.errorCode = static_cast<ErrorCode>(Read32BE(payload.data() + sizeof(uint32_t)));
  out.debugData = payload.subspan(2 * sizeof(uint32_t));

  return FrameParseResult::Ok;
}

FrameParseResult ParseWindowUpdateFrame(std::span<const std::byte> payload, WindowUpdateFrame& out) noexcept {
  if (payload.size() != sizeof(uint32_t)) {
    return FrameParseResult::FrameSizeError;
  }

  out.windowSizeIncrement = Read32BE(payload.data()) & kMaxWindowSize;  // Clear reserved bit
  return FrameParseResult::Ok;
}

void ParseContinuationFrame(FrameHeader header, std::span<const std::byte> payload, ContinuationFrame& out) noexcept {
  out.endHeaders = header.hasFlag(FrameFlags::ContinuationEndHeaders);
  out.headerBlockFragment = payload;
}

// ============================
// Frame writing functions
// ============================

std::byte* PrepareDataFrameGetStartPtr(RawBytes& buffer, uint32_t streamId, uint32_t dataSz, bool endStream) {
  const uint8_t flags = endStream ? FrameFlags::DataEndStream : FrameFlags::None;

  WriteFrame(buffer, FrameType::Data, flags, streamId, dataSz);

  std::byte* pData = buffer.end();

  buffer.addSize(dataSz);

  return pData;
}

std::size_t WriteHeadersFrameWithPriority(RawBytes& buffer, uint32_t streamId, std::span<const std::byte> headerBlock,
                                          uint32_t streamDependency, uint8_t weight, bool exclusive, bool endStream,
                                          bool endHeaders) {
  const std::size_t payloadSize = 5 + headerBlock.size();  // Priority is 5 bytes

  buffer.ensureAvailableCapacityExponential(FrameHeader::kSize + 5UL + headerBlock.size());

  std::byte* pData = buffer.end();

  WriteFrameHeader(pData,
                   FrameHeader(static_cast<uint32_t>(payloadSize), FrameType::Headers,
                               ComputeHeaderFrameFlags(endStream, endHeaders, FrameFlags::HeadersPriority), streamId));
  pData += FrameHeader::kSize;

  // Write priority
  uint32_t depWithExcl = streamDependency;
  if (exclusive) {
    depWithExcl |= 0x80000000U;
  }

  Write32BE(pData, depWithExcl);
  pData += sizeof(uint32_t);

  *pData++ = static_cast<std::byte>(weight);
  pData = Append(headerBlock.data(), headerBlock.size(), pData);

  buffer.setEnd(pData);

  return FrameHeader::kSize + payloadSize;
}

std::size_t WritePriorityFrame(RawBytes& buffer, uint32_t streamId, uint32_t streamDependency, uint8_t weight,
                               bool exclusive) {
  static constexpr std::uint8_t kPayloadSize = 5U;
  const auto ret = WriteFrame(buffer, FrameType::Priority, FrameFlags::None, streamId, kPayloadSize);

  uint32_t depWithExcl = streamDependency;
  if (exclusive) {
    depWithExcl |= 0x80000000U;
  }
  Write32BE(buffer.end(), depWithExcl);
  buffer.end()[sizeof(uint32_t)] = static_cast<std::byte>(weight);

  buffer.addSize(kPayloadSize);
  return ret;
}

std::size_t WriteRstStreamFrame(RawBytes& buffer, uint32_t streamId, ErrorCode errorCode) {
  const auto ret = WriteFrame(buffer, FrameType::RstStream, FrameFlags::None, streamId, sizeof(uint32_t));
  Write32BE(buffer.end(), static_cast<uint32_t>(errorCode));
  buffer.addSize(sizeof(uint32_t));
  return ret;
}

std::size_t WriteSettingsFrame(RawBytes& buffer, std::span<const SettingsEntry> entries) {
  const std::size_t payloadSize = entries.size() * 6;

  buffer.ensureAvailableCapacityExponential(FrameHeader::kSize + payloadSize);

  std::byte* pData = buffer.end();

  WriteFrameHeader(pData, FrameHeader(static_cast<uint32_t>(payloadSize), FrameType::Settings, FrameFlags::None, 0));
  pData += FrameHeader::kSize;

  // Write settings entries
  for (const auto& entry : entries) {
    Write16BE(pData, static_cast<uint16_t>(entry.id));
    pData += sizeof(uint16_t);
    Write32BE(pData, entry.value);
    pData += sizeof(uint32_t);
  }
  buffer.setEnd(pData);

  return FrameHeader::kSize + payloadSize;
}

std::size_t WriteSettingsAckFrame(RawBytes& buffer) {
  return WriteFrame(buffer, FrameType::Settings, FrameFlags::SettingsAck, 0, 0);
}

std::size_t WritePingFrame(RawBytes& buffer, PingFrame pingFrame) {
  const uint8_t flags = pingFrame.isAck ? FrameFlags::PingAck : FrameFlags::None;
  const auto ret = WriteFrame(buffer, FrameType::Ping, flags, 0, sizeof(pingFrame.opaqueData));
  buffer.unchecked_append(pingFrame.opaqueData, sizeof(pingFrame.opaqueData));
  return ret;
}

std::size_t WriteGoAwayFrame(RawBytes& buffer, uint32_t lastStreamId, ErrorCode errorCode, ErrorMsg msg) {
  const auto debugData = ConvertProcessResultErrorMsgToSv(msg);
  const std::size_t payloadSize = 8 + debugData.size();

  buffer.ensureAvailableCapacityExponential(FrameHeader::kSize + 8UL + debugData.size());

  std::byte* pData = buffer.end();

  WriteFrameHeader(pData, FrameHeader(static_cast<uint32_t>(payloadSize), FrameType::GoAway, FrameFlags::None, 0));
  pData += FrameHeader::kSize;

  // Write last stream ID and error code
  Write32BE(pData, lastStreamId);
  pData += sizeof(uint32_t);
  Write32BE(pData, static_cast<uint32_t>(errorCode));
  pData += sizeof(uint32_t);

  // Write debug data
  pData = Append(reinterpret_cast<const std::byte*>(debugData.data()), debugData.size(), pData);
  buffer.setEnd(pData);

  return FrameHeader::kSize + payloadSize;
}

std::size_t WriteWindowUpdateFrame(RawBytes& buffer, uint32_t streamId, uint32_t windowSizeIncrement) {
  const auto ret = WriteFrame(buffer, FrameType::WindowUpdate, FrameFlags::None, streamId, sizeof(uint32_t));
  Write32BE(buffer.end(), windowSizeIncrement & kMaxWindowSize);  // Clear reserved bit
  buffer.addSize(sizeof(uint32_t));
  return ret;
}

}  // namespace aeronet::http2
