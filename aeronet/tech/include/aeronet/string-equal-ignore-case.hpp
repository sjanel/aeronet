#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "aeronet/tolower-str.hpp"
#include "aeronet/toupperlower.hpp"

namespace aeronet {

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || defined(__i386__) || defined(_M_IX86)
#define AERONET_HAS_ASCII_LOWER_EQUAL16 1
// Lowercase + compare 16 bytes at once, entirely in an XMM register: one unaligned load per side, ASCII
// lowercase with the same signed-range trick as AsciiLowerMask (just 16 lanes instead of 8), then
// _mm_cmpeq_epi8 + movemask. Folding two 8-byte AsciiLowerMask steps into one keeps the whole comparison in
// the vector domain and avoids the GPR<->XMM round trips the 8-byte path pays per chunk. SSE2 is part of the
// x86-64 baseline, so no runtime dispatch (and no AVX2 -- 256-bit blocks only pay off past ~64 bytes, which
// header names never reach, and would need a non-inlinable target("avx2") call on this generic build).
inline bool AsciiLowerEqual16(const char* lhs, const char* rhs) {
  const auto vl = _mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs));
  const auto vr = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs));
  const auto aMinus1 = _mm_set1_epi8(static_cast<char>('A' - 1));
  const auto zPlus1 = _mm_set1_epi8(static_cast<char>('Z' + 1));
  const auto lowerBit = _mm_set1_epi8(0x20);
  const auto ll =
      _mm_or_si128(vl, _mm_and_si128(_mm_and_si128(_mm_cmpgt_epi8(vl, aMinus1), _mm_cmpgt_epi8(zPlus1, vl)), lowerBit));
  const auto lr =
      _mm_or_si128(vr, _mm_and_si128(_mm_and_si128(_mm_cmpgt_epi8(vr, aMinus1), _mm_cmpgt_epi8(zPlus1, vr)), lowerBit));
  return _mm_movemask_epi8(_mm_cmpeq_epi8(ll, lr)) == 0xFFFF;
}
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#define AERONET_HAS_ASCII_LOWER_EQUAL16 1
// NEON counterpart of AsciiLowerEqual16, mirroring AsciiLowerMask2's lowercasing exactly.
inline bool AsciiLowerEqual16(const char* lhs, const char* rhs) {
  const auto vl = vld1q_u8(reinterpret_cast<const uint8_t*>(lhs));
  const auto vr = vld1q_u8(reinterpret_cast<const uint8_t*>(rhs));
  const auto aVal = vdupq_n_u8(static_cast<uint8_t>('A'));
  const auto zVal = vdupq_n_u8(static_cast<uint8_t>('Z'));
  const auto lowerBit = vdupq_n_u8(0x20);
  const auto ll = vorrq_u8(vl, vandq_u8(vandq_u8(vcgeq_u8(vl, aVal), vcleq_u8(vl, zVal)), lowerBit));
  const auto lr = vorrq_u8(vr, vandq_u8(vandq_u8(vcgeq_u8(vr, aVal), vcleq_u8(vr, zVal)), lowerBit));
  const auto eq = vreinterpretq_u64_u8(vceqq_u8(ll, lr));
  return (vgetq_lane_u64(eq, 0) & vgetq_lane_u64(eq, 1)) == ~static_cast<uint64_t>(0);
}
#endif

// Case-insensitive (ASCII) equality. Header-name matching is one of the hottest operations in the server, so
// at runtime we lowercase and compare a whole SIMD word at a time with the branchless AsciiLowerMask family
// instead of a per-byte tolower loop: 16 bytes via AsciiLowerEqual16 (SSE2/NEON, available on every x86-64 and
// ARM target), then 8 bytes via AsciiLowerMask for the [8,16) remainder. Each final partial word is handled by
// an overlapping block ([len-16, len) or [len-8, len), in-bounds because len >= the block size), which
// re-checks a few already-equal bytes -- harmless. std::memcpy with a constant size lowers to a single load,
// so each step is one load + one SIMD lowercase + one compare, no call and no per-byte branch. Short inputs
// (len < 8, e.g. "Date"/"Host") and compile-time evaluation use the scalar loop. The SIMD lowercase masks
// exactly [A-Z] for every byte value, matching scalar tolower, so the result is identical to the byte-wise
// comparison.
constexpr bool CaseInsensitiveEqual(std::string_view lhs, std::string_view rhs) {
  const std::size_t len = lhs.size();
  if (len != rhs.size()) {
    return false;
  }

  const char* pLhs = lhs.data();
  const char* pRhs = rhs.data();

  if !consteval {
#ifdef AERONET_HAS_ASCII_LOWER_EQUAL16
    if (len >= 16) {
      std::size_t pos = 0;
      for (; pos + 16 <= len; pos += 16) {
        if (!AsciiLowerEqual16(pLhs + pos, pRhs + pos)) {
          return false;
        }
      }
      if (pos != len) {
        return AsciiLowerEqual16(pLhs + len - 16, pRhs + len - 16);
      }
      return true;
    }
#endif
    if (len >= 8) {
      std::size_t pos = 0;
      for (; pos + 8 <= len; pos += 8) {
        std::uint64_t lw;
        std::uint64_t rw;
        std::memcpy(&lw, pLhs + pos, sizeof(lw));
        std::memcpy(&rw, pRhs + pos, sizeof(rw));
        if (AsciiLowerMask(lw) != AsciiLowerMask(rw)) {
          return false;
        }
      }
      if (pos != len) {
        std::uint64_t lw;
        std::uint64_t rw;
        std::memcpy(&lw, pLhs + len - 8, sizeof(lw));
        std::memcpy(&rw, pRhs + len - 8, sizeof(rw));
        if (AsciiLowerMask(lw) != AsciiLowerMask(rw)) {
          return false;
        }
      }
      return true;
    }
  }

  for (std::size_t i = 0; i < len; ++i) {
    if (tolower(pLhs[i]) != tolower(pRhs[i])) {
      return false;
    }
  }
  return true;
}

#ifdef AERONET_HAS_ASCII_LOWER_EQUAL16
#undef AERONET_HAS_ASCII_LOWER_EQUAL16
#endif

constexpr bool CaseInsensitiveLess(std::string_view lhs, std::string_view rhs) {
  const auto lhsSize = lhs.size();
  const auto rhsSize = rhs.size();
  const auto minSize = lhsSize < rhsSize ? lhsSize : rhsSize;

  const char* pLhs = lhs.data();
  const char* pRhs = rhs.data();

  for (std::size_t i = 0; i < minSize; ++i) {
    const auto lc = tolower(pLhs[i]);
    const auto rc = tolower(pRhs[i]);
    if (lc != rc) {
      return lc < rc;
    }
  }
  return lhsSize < rhsSize;
}

struct CaseInsensitiveHashFunc {
  static constexpr std::size_t operator()(std::string_view str) noexcept {
    // FNV-1a hash, case insensitive
    std::size_t hash = 14695981039346656037ULL;
    for (char ch : str) {
      hash ^= tolower(static_cast<unsigned char>(ch));
      hash *= 1099511628211ULL;
    }
    return hash;
  }
};

struct CaseInsensitiveEqualFunc {
  static constexpr bool operator()(std::string_view lhs, std::string_view rhs) noexcept {
    return CaseInsensitiveEqual(lhs, rhs);
  }
};

}  // namespace aeronet
