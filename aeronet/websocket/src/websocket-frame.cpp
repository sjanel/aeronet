#include "aeronet/websocket-frame.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/websocket-constants.hpp"

namespace aeronet::websocket {

std::size_t FrameHeader::headerSize() const noexcept {
  std::size_t sz = kMinFrameHeaderSize;  // 2 bytes minimum

  if (payloadLength > 0xFFFF) {
    sz += 8;  // 64-bit extended length (payloadLen > 65535)
  } else if (payloadLength >= static_cast<uint64_t>(kPayloadLen16)) {
    sz += 2;  // 16-bit extended length (payloadLen >= 126)
  }

  if (masked) {
    sz += kMaskingKeySize;
  }

  return sz;
}

FrameParseResult ParseFrame(std::span<const std::byte> data, std::size_t maxPayloadSize, bool isServerSide,
                            bool allowRsv1) {
  FrameParseResult result;

  // Need at least 2 bytes for the minimal header
  if (data.size() < kMinFrameHeaderSize) {
    result.status = FrameParseResult::Status::Incomplete;
    return result;
  }

  const auto* ptr = data.data();

  // Parse first byte: FIN, RSV1-3, Opcode
  const std::byte byte0 = ptr[0];
  result.header.fin = (byte0 & kFinBit) != std::byte{0};
  result.header.rsv1 = (byte0 & kRsv1Bit) != std::byte{0};
  result.header.rsv2 = (byte0 & kRsv2Bit) != std::byte{0};
  result.header.rsv3 = (byte0 & kRsv3Bit) != std::byte{0};
  const std::byte rawOpcode = byte0 & kOpcodeMask;

  // Validate RSV bits
  // RSV1 is allowed when permessage-deflate is negotiated (only on data frames)
  // RSV2 and RSV3 must always be 0 unless other extensions are negotiated
  if (result.header.rsv2 || result.header.rsv3) {
    result.status = FrameParseResult::Status::ProtocolError;
    result.errorMessage = "Reserved bits RSV2/RSV3 must be 0";
    return result;
  }

  // RSV1 check: allowed on data frames when permessage-deflate is active
  if (result.header.rsv1) {
    if (!allowRsv1) {
      result.status = FrameParseResult::Status::ProtocolError;
      result.errorMessage = "RSV1 bit set without permessage-deflate extension";
      return result;
    }
    // RSV1 must not be set on control frames per RFC 7692
    const auto opcodeVal = rawOpcode;
    if (opcodeVal >= std::byte{0x08}) {  // Control frames
      result.status = FrameParseResult::Status::ProtocolError;
      result.errorMessage = "RSV1 bit must not be set on control frames";
      return result;
    }
  }

  // Validate opcode
  if (IsReservedOpcode(rawOpcode)) {
    result.status = FrameParseResult::Status::ProtocolError;
    result.errorMessage = "Reserved opcode";
    return result;
  }
  result.header.opcode = static_cast<Opcode>(rawOpcode);

  // Control frames must have FIN set
  if (IsControlFrame(result.header.opcode) && !result.header.fin) {
    result.status = FrameParseResult::Status::ProtocolError;
    result.errorMessage = "Control frames must not be fragmented";
    return result;
  }

  // Parse second byte: MASK, Payload length (7 bits)
  const std::byte byte1 = ptr[1];
  result.header.masked = (byte1 & kMaskBit) != std::byte{0};
  std::byte payloadLen7 = byte1 & kPayloadLenMask;

  // Validate masking based on direction
  if (isServerSide) {
    // Server receiving from client: MUST be masked
    if (!result.header.masked) {
      result.status = FrameParseResult::Status::ProtocolError;
      result.errorMessage = "Client frames must be masked";
      return result;
    }
  } else {
    // Client receiving from server: MUST NOT be masked
    if (result.header.masked) {
      result.status = FrameParseResult::Status::ProtocolError;
      result.errorMessage = "Server frames must not be masked";
      return result;
    }
  }

  // Calculate header size and parse extended length
  std::size_t headerSize = kMinFrameHeaderSize;
  std::size_t offset = 2;

  if (payloadLen7 == kPayloadLen16) {
    // 16-bit extended length
    headerSize += 2;
    if (data.size() < headerSize) {
      result.status = FrameParseResult::Status::Incomplete;
      return result;
    }
    // Network byte order (big-endian)
    result.header.payloadLength = (static_cast<uint64_t>(ptr[offset]) << 8) | static_cast<uint64_t>(ptr[offset + 1]);
    offset += 2;

    // RFC 6455 §5.2: the minimal number of bytes MUST be used to encode the length
    if (result.header.payloadLength < static_cast<uint64_t>(kPayloadLen16)) {
      result.status = FrameParseResult::Status::ProtocolError;
      result.errorMessage = "Non-minimal extended length encoding";
      return result;
    }
  } else if (payloadLen7 == kPayloadLen64) {
    // 64-bit extended length
    headerSize += 8;
    if (data.size() < headerSize) {
      result.status = FrameParseResult::Status::Incomplete;
      return result;
    }
    // Network byte order (big-endian)
    result.header.payloadLength = 0;
    for (std::size_t idx = 0; idx < 8; ++idx) {
      result.header.payloadLength = (result.header.payloadLength << 8) | static_cast<uint64_t>(ptr[offset + idx]);
    }
    offset += 8;

    // RFC 6455 §5.2: MSB must be 0 (payload length is unsigned)
    if ((result.header.payloadLength >> 63) != 0) {
      result.status = FrameParseResult::Status::ProtocolError;
      result.errorMessage = "Invalid payload length (MSB set)";
      return result;
    }

    // RFC 6455 §5.2: the minimal number of bytes MUST be used
    if (result.header.payloadLength <= 0xFFFF) {
      result.status = FrameParseResult::Status::ProtocolError;
      result.errorMessage = "Non-minimal extended length encoding";
      return result;
    }
  } else {
    result.header.payloadLength = static_cast<uint64_t>(payloadLen7);
  }

  // Control frames have a max payload size of 125
  if (IsControlFrame(result.header.opcode) && result.header.payloadLength > kMaxControlFramePayload) {
    result.status = FrameParseResult::Status::ProtocolError;
    result.errorMessage = "Control frame payload too large";
    return result;
  }

  // Check against configured maximum
  if (maxPayloadSize > 0 && result.header.payloadLength > maxPayloadSize) {
    result.status = FrameParseResult::Status::PayloadTooLarge;
    result.errorMessage = "Payload exceeds maximum size";
    return result;
  }

  // Parse masking key if present
  if (result.header.masked) {
    headerSize += kMaskingKeySize;
    if (data.size() < headerSize) {
      result.status = FrameParseResult::Status::Incomplete;
      return result;
    }
    std::memcpy(&result.header.maskingKey, ptr + offset, kMaskingKeySize);
    offset += kMaskingKeySize;
  }

  // Check if we have the complete payload
  const std::size_t totalFrameSize = headerSize + result.header.payloadLength;
  if (data.size() < totalFrameSize) {
    result.status = FrameParseResult::Status::Incomplete;
    return result;
  }

  // Frame complete!
  result.status = FrameParseResult::Status::Complete;
  result.payload = data.subspan(offset, static_cast<std::size_t>(result.header.payloadLength));
  result.bytesConsumed = totalFrameSize;

  return result;
}

void ApplyMask(std::span<std::byte> data, MaskingKey maskingKey) {
  // XOR each byte with the corresponding masking key byte (rotating)
  // Optimized version processes 8 bytes at a time when possible
  const std::size_t sz = data.size();
  auto* bytes = data.data();

  // Create a 64-bit mask for bulk processing
  // Note: we need the mask to match byte positions in memory, not a specific endianness.
  // Since we're using memcpy (which preserves byte order), construct mask as bytes would appear.
  std::size_t idx = 0;
  if (sz >= 8) {
    const uint64_t mask64 = static_cast<uint64_t>(maskingKey) | (static_cast<uint64_t>(maskingKey) << 32);

    for (; idx + 8 <= sz; idx += 8) {
      uint64_t chunk;
      std::memcpy(&chunk, bytes + idx, sizeof(chunk));
      chunk ^= mask64;
      std::memcpy(bytes + idx, &chunk, sizeof(chunk));
    }
  }
  for (; idx < sz; ++idx) {
    bytes[idx] ^= static_cast<std::byte>((maskingKey >> ((idx & 3) * 8)) & 0xFF);
  }
}

void BuildFrame(RawBytes& output, Opcode opcode, std::span<const std::byte> payload, bool fin, bool shouldMask,
                MaskingKey maskingKey, bool rsv1) {
  const std::size_t payloadSize = payload.size();

  // Calculate header size
  std::size_t headerSize = kMinFrameHeaderSize;
  if (payloadSize >= static_cast<uint64_t>(kPayloadLen64)) {
    headerSize += 8;
  } else if (payloadSize >= static_cast<uint64_t>(kPayloadLen16)) {
    headerSize += 2;
  }
  if (shouldMask) {
    headerSize += kMaskingKeySize;
  }

  output.reserve(output.size() + headerSize + payloadSize);

  // First byte: FIN | RSV1-3 | Opcode
  std::byte byte0 = static_cast<std::byte>(opcode);
  if (fin) {
    byte0 |= kFinBit;
  }
  if (rsv1) {
    byte0 |= kRsv1Bit;
  }
  output.push_back(byte0);

  // Second byte: MASK | Payload length (7 bits or indicator)
  std::byte byte1{};
  if (shouldMask) {
    byte1 |= kMaskBit;
  }

  if (payloadSize < static_cast<uint64_t>(kPayloadLen16)) {
    byte1 |= static_cast<std::byte>(payloadSize);
    output.push_back(byte1);
  } else if (payloadSize <= 0xFFFF) {
    byte1 |= kPayloadLen16;
    output.push_back(byte1);
    // 16-bit big-endian length
    output.push_back(static_cast<std::byte>((payloadSize >> 8) & 0xFF));
    output.push_back(static_cast<std::byte>(payloadSize & 0xFF));
  } else {
    byte1 |= kPayloadLen64;
    output.push_back(byte1);
    // 64-bit big-endian length
    for (int idx = 7; idx >= 0; --idx) {
      output.push_back(static_cast<std::byte>((payloadSize >> (idx * 8)) & 0xFF));
    }
  }

  // Masking key (if masking)
  if (shouldMask) {
    output.append(reinterpret_cast<const std::byte*>(&maskingKey), kMaskingKeySize);
  }

  // Payload (masked if needed)
  if (!payload.empty()) {
    if (shouldMask) {
      // Need to mask the payload before appending
      const std::size_t payloadStart = output.size();
      output.append(payload.data(), payloadSize);
      ApplyMask(std::as_writable_bytes(std::span(output.data() + payloadStart, payloadSize)), maskingKey);
    } else {
      output.append(payload.data(), payloadSize);
    }
  }
}

void BuildCloseFrame(RawBytes& output, CloseCode code, std::string_view reason, bool shouldMask,
                     MaskingKey maskingKey) {
  // Close frame payload: 2-byte status code (big-endian) + optional reason
  RawBytes closePayload;

  if (code != CloseCode::NoStatusReceived) {
    const auto codeVal = static_cast<uint16_t>(code);
    closePayload.push_back(static_cast<std::byte>((codeVal >> 8) & 0xFF));
    closePayload.push_back(static_cast<std::byte>(codeVal & 0xFF));

    if (!reason.empty()) {
      // Truncate reason if it would exceed control frame limit
      const std::size_t maxReasonLen = kMaxControlFramePayload - 2;
      if (reason.size() > maxReasonLen) {
        reason = reason.substr(0, maxReasonLen);
      }
      closePayload.append(std::as_bytes(std::span(reason)));
    }
  }

  BuildFrame(output, Opcode::Close, std::span<const std::byte>(closePayload), true, shouldMask, maskingKey);
}

ClosePayload ParseClosePayload(std::span<const std::byte> payload) {
  ClosePayload result;

  if (payload.size() >= 2) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(payload.data());
    result.code = static_cast<CloseCode>((static_cast<uint16_t>(bytes[0]) << 8) | static_cast<uint16_t>(bytes[1]));

    if (payload.size() > 2) {
      result.reason = std::string_view(reinterpret_cast<const char*>(payload.data() + 2), payload.size() - 2);
    }
  } else if (payload.empty()) {
    result.code = CloseCode::NoStatusReceived;
  } else {
    // 1 byte is invalid per RFC 6455
    result.code = CloseCode::ProtocolError;
  }

  return result;
}

}  // namespace aeronet::websocket
