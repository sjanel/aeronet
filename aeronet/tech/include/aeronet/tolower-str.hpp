#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "aeronet/toupperlower.hpp"

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#endif

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace aeronet {

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif

constexpr uint64_t AsciiLowerMask(uint64_t val) {
  auto BytewiseLower = [](uint64_t input) {
    uint64_t result = 0;
    for (std::size_t bytePos = 0; bytePos < sizeof(uint64_t); ++bytePos) {
      const auto shift = static_cast<unsigned>(bytePos * 8);
      const auto byteVal = static_cast<unsigned char>(input >> shift);
      const auto lowerVal = tolower(byteVal);
      result |= static_cast<uint64_t>(lowerVal) << shift;
    }
    return result;
  };

  if consteval {
    return BytewiseLower(val);
  }

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
  const auto input = _mm_cvtsi64_si128(static_cast<long long>(val));
  const auto aMinus1 = _mm_set1_epi8(static_cast<char>('A' - 1));
  const auto zPlus1 = _mm_set1_epi8(static_cast<char>('Z' + 1));
  const auto geA = _mm_cmpgt_epi8(input, aMinus1);
  const auto ltZ = _mm_cmpgt_epi8(zPlus1, input);
  const auto isUpper = _mm_and_si128(geA, ltZ);
  const auto lowerBit = _mm_and_si128(isUpper, _mm_set1_epi8(0x20));
  const auto lowered = _mm_or_si128(input, lowerBit);
  return static_cast<uint64_t>(_mm_cvtsi128_si64(lowered));
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  const auto input = vreinterpret_u8_u64(vdup_n_u64(val));
  const auto aVal = vdup_n_u8(static_cast<uint8_t>('A'));
  const auto zVal = vdup_n_u8(static_cast<uint8_t>('Z'));
  const auto geA = vcge_u8(input, aVal);
  const auto leZ = vcle_u8(input, zVal);
  const auto isUpper = vand_u8(geA, leZ);
  const auto lowerBit = vand_u8(isUpper, vdup_n_u8(0x20));
  const auto lowered = vorr_u8(input, lowerBit);
  return static_cast<uint64_t>(vget_lane_u64(vreinterpret_u64_u8(lowered), 0));
#else
  return BytewiseLower(val);
#endif
}

#if defined(__AVX2__)
inline void AsciiLowerMask4(const uint64_t* src, uint64_t* dst) {
  const auto input = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
  const auto aMinus1 = _mm256_set1_epi8(static_cast<char>('A' - 1));
  const auto zPlus1 = _mm256_set1_epi8(static_cast<char>('Z' + 1));
  const auto geA = _mm256_cmpgt_epi8(input, aMinus1);
  const auto ltZ = _mm256_cmpgt_epi8(zPlus1, input);
  const auto isUpper = _mm256_and_si256(geA, ltZ);
  const auto lowerBit = _mm256_and_si256(isUpper, _mm256_set1_epi8(0x20));
  const auto lowered = _mm256_or_si256(input, lowerBit);
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), lowered);
}

inline void AsciiLowerMask4(uint64_t* v) { AsciiLowerMask4(static_cast<const uint64_t*>(v), v); }
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
inline void AsciiLowerMask2(const uint64_t* src, uint64_t* dst) {
  const auto input = vld1q_u8(reinterpret_cast<const uint8_t*>(src));
  const auto aVal = vdupq_n_u8(static_cast<uint8_t>('A'));
  const auto zVal = vdupq_n_u8(static_cast<uint8_t>('Z'));
  const auto geA = vcgeq_u8(input, aVal);
  const auto leZ = vcleq_u8(input, zVal);
  const auto isUpper = vandq_u8(geA, leZ);
  const auto lowerBit = vandq_u8(isUpper, vdupq_n_u8(0x20));
  const auto lowered = vorrq_u8(input, lowerBit);
  vst1q_u8(reinterpret_cast<uint8_t*>(dst), lowered);
}

inline void AsciiLowerMask2(uint64_t* v) { AsciiLowerMask2(static_cast<const uint64_t*>(v), v); }
#endif

// Inplace optimized tolower for ASCII characters
// buf should be at least of size 'len'.
constexpr void tolower(char* buf, std::size_t len) {
  std::size_t charPos = 0;

  static constexpr std::size_t kWordAlign = alignof(std::uint64_t);

  const auto misalignment = reinterpret_cast<std::uintptr_t>(buf) % kWordAlign;
  uint64_t head;
  if consteval {
    head = len;
  } else {
    if (misalignment == 0) {
      head = 0;
    } else {
      const auto delta = static_cast<std::size_t>(kWordAlign - misalignment);
      if (len < delta) {
        head = len;
      } else {
        head = delta;
      }
    }
  }

  for (; charPos < head; ++charPos) {
    buf[charPos] = tolower(buf[charPos]);
  }

  if consteval {
    return;
  }

#if defined(__AVX2__)
  // Process 32 bytes at a time
  for (; charPos + 32 <= len; charPos += 32) {
    auto* chunk = reinterpret_cast<uint64_t*>(buf + charPos);
    AsciiLowerMask4(chunk);
  }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  // Process 16 bytes at a time
  for (; charPos + 16 <= len; charPos += 16) {
    auto* chunk = reinterpret_cast<uint64_t*>(buf + charPos);
    AsciiLowerMask2(chunk);
  }
#endif

  // Process 8 bytes at a time
  for (; charPos + 8 <= len; charPos += sizeof(uint64_t)) {
    auto* chunk = reinterpret_cast<uint64_t*>(buf + charPos);
    *chunk = AsciiLowerMask(*chunk);
  }

  // tail
  for (; charPos < len; ++charPos) {
    buf[charPos] = tolower(buf[charPos]);
  }
}

// Apply tolower from 'from' to 'to' for len bytes.
// from and to buffers should be at least of size 'len'.
constexpr void tolower_n(const char* from, std::size_t len, char* to) {
  std::size_t pos = 0;
  static constexpr std::size_t kAlign = alignof(std::uint64_t);

  if consteval {
    for (; pos < len; ++pos) {
      to[pos] = tolower(from[pos]);
    }
    return;
  }

  /*
   * Why no AVX2/NEON here?
   * Because they require contiguous aligned loads/stores of both input and output.
   * Without a temporary buffer, you cannot safely vectorize cross-buffer.
   */

  // Align both pointers if they share the same relative misalignment
  const auto fromMisalign = reinterpret_cast<std::uintptr_t>(from) % kAlign;
  const auto toMisalign = reinterpret_cast<std::uintptr_t>(to) % kAlign;

  if (fromMisalign == toMisalign && fromMisalign != 0) {
    const auto head = std::min(len, static_cast<std::size_t>(kAlign - fromMisalign));
    for (; pos < head; ++pos) {
      to[pos] = tolower(from[pos]);
    }
  }

  // 8-byte fast path
  if (fromMisalign == toMisalign) {
    // Both aligned: direct reinterpret_cast (fastest)
    for (; pos + 8 <= len; pos += 8) {
      const auto* in = reinterpret_cast<const uint64_t*>(from + pos);
      auto* out = reinterpret_cast<uint64_t*>(to + pos);
      *out = AsciiLowerMask(*in);
    }
  } else {
    // Misaligned: use memcpy (still much better than scalar)
    for (; pos + 8 <= len; pos += 8) {
      uint64_t val;
      std::memcpy(&val, from + pos, sizeof(uint64_t));
      val = AsciiLowerMask(val);
      std::memcpy(to + pos, &val, sizeof(uint64_t));
    }
  }

  // tail
  for (; pos < len; ++pos) {
    to[pos] = tolower(from[pos]);
  }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

}  // namespace aeronet
