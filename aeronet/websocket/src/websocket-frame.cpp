#include "aeronet/websocket-frame.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "aeronet/avx2.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/websocket-constants.hpp"

// On x86-64, always include the full intrinsic header so both SSE2 and AVX2
// code paths can be compiled.  The AVX2 path is gated by a target attribute
// and selected at runtime via __builtin_cpu_supports, so -mavx2 is NOT needed
// at the TU level.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#ifdef AERONET_WINDOWS
#include <intrin.h>
#endif
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace aeronet::websocket {

std::size_t FrameHeader::headerSize() const noexcept {
  std::size_t sz = kMinFrameHeaderSize;  // 2 bytes minimum

  if (payloadLength > 0xFFFFU) {
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
    result.pErrorMessage = "Reserved bits RSV2/RSV3 must be 0";
    return result;
  }

  // RSV1 check: allowed on data frames when permessage-deflate is active
  if (result.header.rsv1) {
    if (!allowRsv1) {
      result.status = FrameParseResult::Status::ProtocolError;
      result.pErrorMessage = "RSV1 bit set without permessage-deflate extension";
      return result;
    }
    // RSV1 must not be set on control frames per RFC 7692
    const auto opcodeVal = rawOpcode;
    if (opcodeVal >= std::byte{0x08}) {  // Control frames
      result.status = FrameParseResult::Status::ProtocolError;
      result.pErrorMessage = "RSV1 bit must not be set on control frames";
      return result;
    }
  }

  // Validate opcode
  if (IsReservedOpcode(rawOpcode)) {
    result.status = FrameParseResult::Status::ProtocolError;
    result.pErrorMessage = "Reserved opcode";
    return result;
  }
  result.header.opcode = static_cast<Opcode>(rawOpcode);

  // Control frames must have FIN set
  if (IsControlFrame(result.header.opcode) && !result.header.fin) {
    result.status = FrameParseResult::Status::ProtocolError;
    result.pErrorMessage = "Control frames must not be fragmented";
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
      result.pErrorMessage = "Client frames must be masked";
      return result;
    }
  } else {
    // Client receiving from server: MUST NOT be masked
    if (result.header.masked) {
      result.status = FrameParseResult::Status::ProtocolError;
      result.pErrorMessage = "Server frames must not be masked";
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
    result.header.payloadLength = (static_cast<uint64_t>(ptr[offset]) << 8U) | static_cast<uint64_t>(ptr[offset + 1]);
    offset += 2;

    // RFC 6455 §5.2: the minimal number of bytes MUST be used to encode the length
    if (result.header.payloadLength < static_cast<uint64_t>(kPayloadLen16)) {
      result.status = FrameParseResult::Status::ProtocolError;
      result.pErrorMessage = "Non-minimal extended length encoding";
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
      result.header.payloadLength = (result.header.payloadLength << 8U) | static_cast<uint64_t>(ptr[offset + idx]);
    }
    offset += 8;

    // RFC 6455 §5.2: MSB must be 0 (payload length is unsigned)
    if ((result.header.payloadLength >> 63U) != 0) {
      result.status = FrameParseResult::Status::ProtocolError;
      result.pErrorMessage = "Invalid payload length (MSB set)";
      return result;
    }

    // RFC 6455 §5.2: the minimal number of bytes MUST be used
    if (result.header.payloadLength <= 0xFFFF) {
      result.status = FrameParseResult::Status::ProtocolError;
      result.pErrorMessage = "Non-minimal extended length encoding";
      return result;
    }
  } else {
    result.header.payloadLength = static_cast<uint64_t>(payloadLen7);
  }

  // Control frames have a max payload size of 125
  if (IsControlFrame(result.header.opcode) && result.header.payloadLength > kMaxControlFramePayload) {
    result.status = FrameParseResult::Status::ProtocolError;
    result.pErrorMessage = "Control frame payload too large";
    return result;
  }

  // Check against configured maximum
  if (maxPayloadSize > 0 && result.header.payloadLength > maxPayloadSize) {
    result.status = FrameParseResult::Status::PayloadTooLarge;
    result.pErrorMessage = "Payload exceeds maximum size";
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

// The masking functions mask src into dst, which may be src itself (in-place masking) or another buffer (masked copy).

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
namespace detail {

namespace {

std::size_t MaskSse2(const std::byte* src, std::byte* dst, std::size_t sz, MaskingKey maskingKey) {
  std::size_t idx = 0;
  if (sz >= 16) {
    const __m128i mask128 = _mm_set1_epi32(static_cast<int>(maskingKey));
    for (; idx + 16 <= sz; idx += 16) {
      const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + idx));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + idx), _mm_xor_si128(chunk, mask128));
    }
  }
  return idx;
}

// AVX2 path: 32-byte XOR loop with SSE2 residue for 16-byte remainder.
// The target attribute makes GCC/Clang emit AVX2 instructions for this
// function only, without requiring -mavx2 for the whole TU.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif

std::size_t MaskAvx2(const std::byte* src, std::byte* dst, std::size_t sz, MaskingKey maskingKey) {
  std::size_t idx = 0;
  if (sz >= 32) {
    const __m256i mask256 = _mm256_set1_epi32(static_cast<int>(maskingKey));
    for (; idx + 32 <= sz; idx += 32) {
      const __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + idx));
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + idx), _mm256_xor_si256(chunk, mask256));
    }
  }
  // Handle remainder (< 32 bytes) with SSE2, always available on x86-64.
  idx += MaskSse2(src + idx, dst + idx, sz - idx, maskingKey);
  return idx;
}

}  // namespace

}  // namespace detail
#endif

namespace {

// The masking key over 8 bytes, as a native word: the mask of the 8 bytes starting at a multiple of 4 from the start of
// the payload (the same on both endiannesses, its two halves being equal).
constexpr uint64_t MaskingKey64(MaskingKey maskingKey) noexcept {
  return static_cast<uint64_t>(maskingKey) | (static_cast<uint64_t>(maskingKey) << 32U);
}

// The key byte masking the byte 'offset' (< 4) bytes after a multiple of 4 from the start of the payload.
constexpr std::byte KeyByte(MaskingKey maskingKey, std::size_t offset) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    return static_cast<std::byte>(maskingKey >> (offset * 8U));
  } else {
    return static_cast<std::byte>(maskingKey >> ((kMaskingKeySize - 1U - offset) * 8U));
  }
}

// Masks the size bytes of src (from sizeof(T) to 2 * sizeof(T), at a multiple of 4 from the start of the payload) into
// dst with two overlapping words, both loaded before being stored.
template <class T>
void MaskTwoWords(const std::byte* src, std::byte* dst, std::size_t size, uint64_t mask64) noexcept {
  T first;
  T last;
  const std::size_t offset = size - sizeof(T);
  std::memcpy(&first, src, sizeof(T));
  std::memcpy(&last, src + offset, sizeof(T));
  first ^= static_cast<T>(mask64);
  const auto shift = static_cast<int>((offset % kMaskingKeySize) * 8U);
  // The mask (from MaskingKey64) of the bytes starting 'offset' bytes after a multiple of 4 from the start of the
  // payload. Its two halves are still equal: its low 32 bits mask the 4 bytes starting there.
  if constexpr (std::endian::native == std::endian::little) {
    last ^= static_cast<T>(std::rotr(mask64, shift));
  } else {
    last ^= static_cast<T>(std::rotl(mask64, shift));
  }
  std::memcpy(dst, &first, sizeof(T));
  std::memcpy(dst + offset, &last, sizeof(T));
}

// Masks the size bytes of src, starting at the start of the payload, into dst.
void MaskBytes(const std::byte* src, std::byte* dst, std::size_t size, MaskingKey maskingKey) {
  // XOR each byte with the repeating 4-byte masking key.
  // On x86-64 the AVX2 path is selected at runtime via CPUID; the SSE2 path
  // is the baseline fallback.  ARM uses NEON unconditionally.
  // The loops process multiples of 8 bytes from the start of the payload, so the broadcast of the 32-bit key into wider
  // registers needs no rotation, and neither does the tail. A payload of less than 16 bytes is all tail.
  std::size_t idx = 0;

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  if (size >= 16) {
    idx = HasAvx2() ? detail::MaskAvx2(src, dst, size, maskingKey) : detail::MaskSse2(src, dst, size, maskingKey);
  }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  if (size >= 16) {
    const uint8x16_t mask128 = vreinterpretq_u8_u32(vdupq_n_u32(maskingKey));
    for (; idx + 16 <= size; idx += 16) {
      const uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(src + idx));
      vst1q_u8(reinterpret_cast<uint8_t*>(dst + idx), veorq_u8(chunk, mask128));
    }
  }
#else
  // 64-bit scalar fallback for architectures without 128-bit SIMD
  const uint64_t mask64 = MaskingKey64(maskingKey);
  for (; idx + 8 <= size; idx += 8) {
    uint64_t chunk;
    std::memcpy(&chunk, src + idx, sizeof(chunk));
    chunk ^= mask64;
    std::memcpy(dst + idx, &chunk, sizeof(chunk));
  }
#endif

  // Tail of at most 15 bytes (7 for scalar): two overlapping words from 4 bytes, rather than byte per byte.
  const std::size_t tailSize = size - idx;
  src += idx;
  dst += idx;
  if (tailSize >= sizeof(uint32_t)) {
    if (tailSize >= sizeof(uint64_t)) {
      MaskTwoWords<uint64_t>(src, dst, tailSize, MaskingKey64(maskingKey));
    } else {
      MaskTwoWords<uint32_t>(src, dst, tailSize, MaskingKey64(maskingKey));
    }
  } else {
    for (std::size_t pos = 0; pos < tailSize; ++pos) {
      dst[pos] = src[pos] ^ KeyByte(maskingKey, pos);
    }
  }
}

}  // namespace

void ApplyMask(std::span<std::byte> data, MaskingKey maskingKey) {
  MaskBytes(data.data(), data.data(), data.size(), maskingKey);
}

std::byte* AppendMasked(std::span<const std::byte> src, std::byte* dst, MaskingKey maskingKey) {
  MaskBytes(src.data(), dst, src.size(), maskingKey);
  return dst + src.size();
}

namespace {

// Appends the frame header, reserving the capacity for the reservedPayloadSize bytes of payload appended next.
inline void AppendFrameHeader(RawBytes& output, Opcode opcode, std::size_t payloadSize, bool fin, bool shouldMask,
                              MaskingKey maskingKey, bool rsv1, std::size_t reservedPayloadSize) {
  // Calculate exact header size:
  //   2 bytes minimum (byte0 + byte1)
  //   +2 bytes if payload requires 16-bit extended length (126–65535)
  //   +8 bytes if payload requires 64-bit extended length (>65535)
  //   +4 bytes masking key (if shouldMask)
  std::size_t headerSize = kMinFrameHeaderSize;

  if (payloadSize >= static_cast<uint64_t>(kPayloadLen16) && payloadSize <= 0xFFFF) {
    headerSize += 2;
  } else if (payloadSize > 0xFFFF) {
    headerSize += 8;
  }

  if (shouldMask) {
    headerSize += kMaskingKeySize;
  }

  output.ensureAvailableCapacity(headerSize + reservedPayloadSize);

  // First byte: FIN | RSV1-3 | Opcode
  std::byte byte0 = static_cast<std::byte>(opcode);
  if (fin) {
    byte0 |= kFinBit;
  }
  if (rsv1) {
    byte0 |= kRsv1Bit;
  }
  output.unchecked_push_back(byte0);

  // Second byte: MASK | Payload length (7 bits or indicator)
  std::byte byte1{};
  if (shouldMask) {
    byte1 |= kMaskBit;
  }

  if (payloadSize < static_cast<uint64_t>(kPayloadLen16)) {
    byte1 |= static_cast<std::byte>(payloadSize);
    output.unchecked_push_back(byte1);
  } else if (payloadSize <= 0xFFFF) {
    byte1 |= kPayloadLen16;
    output.unchecked_push_back(byte1);
    // 16-bit big-endian length
    output.unchecked_push_back(static_cast<std::byte>((payloadSize >> 8U) & 0xFFU));
    output.unchecked_push_back(static_cast<std::byte>(payloadSize & 0xFFU));
  } else {
    byte1 |= kPayloadLen64;
    output.unchecked_push_back(byte1);
    // 64-bit big-endian length
    for (int idx = 7; idx >= 0; --idx) {
      output.unchecked_push_back(static_cast<std::byte>((payloadSize >> static_cast<uint8_t>(idx * 8)) & 0xFFU));
    }
  }

  // Masking key (if masking)
  if (shouldMask) {
    output.unchecked_append(reinterpret_cast<const std::byte*>(&maskingKey), kMaskingKeySize);
  }
}

inline void AppendFramePayload(RawBytes& output, std::span<const std::byte> payload, bool shouldMask,
                               MaskingKey maskingKey) {
  // Payload (masked if needed, in the same pass as copied)
  if (!payload.empty()) {
    if (shouldMask) {
      output.setEnd(AppendMasked(payload, output.data() + output.size(), maskingKey));
    } else {
      output.unchecked_append(payload);
    }
  }
}

}  // namespace

void BuildFrame(RawBytes& output, Opcode opcode, std::span<const std::byte> payload, bool fin, bool shouldMask,
                MaskingKey maskingKey, bool rsv1) {
  const std::size_t payloadSize = payload.size();

  AppendFrameHeader(output, opcode, payloadSize, fin, shouldMask, maskingKey, rsv1, payloadSize);
  AppendFramePayload(output, payload, shouldMask, maskingKey);
}

void BuildFrameHeader(RawBytes& output, Opcode opcode, std::size_t payloadSize, bool shouldMask, MaskingKey maskingKey,
                      bool rsv1) {
  AppendFrameHeader(output, opcode, payloadSize, true, shouldMask, maskingKey, rsv1, 0);
}

void BuildCloseFrame(RawBytes& output, CloseCode code, std::string_view reason, bool shouldMask,
                     MaskingKey maskingKey) {
  // Close frame payload: 2-byte status code (big-endian) + optional reason
  std::size_t payloadSize = 0;
  std::size_t reasonLen = 0;
  if (code != CloseCode::NoStatusReceived) {
    if (reason.empty()) {
      payloadSize = 2UL;
    } else {
      // Truncate reason if it would exceed control frame limit
      const std::size_t maxReasonLen = kMaxControlFramePayload - 2;
      reasonLen = std::min(reason.size(), maxReasonLen);
      payloadSize = 2UL + reasonLen;
    }
  }

  AppendFrameHeader(output, Opcode::Close, payloadSize, true, shouldMask, maskingKey, false, payloadSize);

  if (payloadSize != 0) {
    const auto codeVal = static_cast<uint16_t>(code);

    output.unchecked_push_back(static_cast<std::byte>(static_cast<uint8_t>(codeVal >> 8U) & 0xFFU));
    output.unchecked_push_back(static_cast<std::byte>(codeVal & 0xFFU));

    output.unchecked_append(reinterpret_cast<const std::byte*>(reason.data()), reasonLen);
    if (shouldMask) {
      ApplyMask(std::as_writable_bytes(std::span(output.end() - payloadSize, payloadSize)), maskingKey);
    }
  }
}

ClosePayload ParseClosePayload(std::span<const std::byte> payload) {
  ClosePayload result;

  if (payload.size() >= 2) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(payload.data());
    result.code = static_cast<CloseCode>(static_cast<uint16_t>(static_cast<uint16_t>(bytes[0]) << 8U) |
                                         static_cast<uint16_t>(bytes[1]));

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
