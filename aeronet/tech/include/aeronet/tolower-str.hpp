#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "aeronet/toupperlower.hpp"

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#endif

namespace aeronet {

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

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
  if consteval {
    return BytewiseLower(val);
  } else {
    const auto input = _mm_cvtsi64_si128(static_cast<long long>(val));
    const auto aMinus1 = _mm_set1_epi8(static_cast<char>('A' - 1));
    const auto zPlus1 = _mm_set1_epi8(static_cast<char>('Z' + 1));
    const auto geA = _mm_cmpgt_epi8(input, aMinus1);
    const auto ltZ = _mm_cmpgt_epi8(zPlus1, input);
    const auto isUpper = _mm_and_si128(geA, ltZ);
    const auto lowerBit = _mm_and_si128(isUpper, _mm_set1_epi8(0x20));
    const auto lowered = _mm_or_si128(input, lowerBit);
    return static_cast<uint64_t>(_mm_cvtsi128_si64(lowered));
  }
#else
  return BytewiseLower(val);
#endif
}

// Inplace optimized tolower for ASCII characters
// buf should be at least of size 'len'.
constexpr void tolower(char *buf, std::size_t len) {
  std::size_t charPos = 0;

  // Process 8 bytes at a time
  // TODO: check if we can avoid val by checking alignment of buf
  for (; charPos + 8 <= len; charPos += sizeof(uint64_t)) {
    uint64_t val;
    std::memcpy(&val, buf + charPos, sizeof(uint64_t));
    val = AsciiLowerMask(val);
    std::memcpy(buf + charPos, &val, sizeof(uint64_t));
  }

  // Tail (0–7 bytes)
  for (; charPos < len; ++charPos) {
    buf[charPos] = tolower(buf[charPos]);
  }
}

// Apply tolower from 'from' to 'to' for len bytes.
// from and to buffers should be at least of size 'len'.
constexpr void tolower_n(const char *from, std::size_t len, char *to) {
  std::size_t charPos = 0;

  // Process 8 bytes at a time
  for (; charPos + 8 <= len; charPos += sizeof(uint64_t)) {
    uint64_t val;
    std::memcpy(&val, from + charPos, sizeof(uint64_t));
    val = AsciiLowerMask(val);
    std::memcpy(to + charPos, &val, sizeof(uint64_t));
  }

  // Tail (0–7 bytes)
  for (; charPos < len; ++charPos) {
    to[charPos] = tolower(from[charPos]);
  }
}

}  // namespace aeronet
