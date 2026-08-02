#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "aeronet/ascii-lower-mask.hpp"
#include "aeronet/compiler-config.hpp"
#include "aeronet/toupperlower.hpp"

namespace aeronet {

// Inplace optimized tolower for ASCII characters
// buf should be at least of size 'len'.
constexpr void tolower(char* buf, std::size_t len) {
  if consteval {
    for (std::size_t charPos = 0; charPos < len; ++charPos) {
      buf[charPos] = tolower(buf[charPos]);
    }
    return;
  }

  std::size_t charPos = 0;

  static constexpr std::size_t kWordAlign = alignof(std::uint64_t);

  const auto misalignment = reinterpret_cast<std::uintptr_t>(buf) % kWordAlign;
  std::size_t head;
  if (misalignment == 0) {
    head = 0;
  } else {
    const auto delta = static_cast<std::size_t>(kWordAlign - misalignment);
    head = len < delta ? len : delta;
  }

  for (; charPos < head; ++charPos) {
    buf[charPos] = tolower(buf[charPos]);
  }

#ifdef AERONET_HAS_ASCII_LOWER_MASK4
  if (HasAvx2ForToLower()) {
    // Process 32 bytes at a time when AVX2 is available.
    for (; charPos + 32 <= len; charPos += 32) {
      auto* chunk = reinterpret_cast<uint64_t*>(buf + charPos);
      AsciiLowerMask4(chunk);
    }
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
constexpr void tolower_n(const char* from, std::size_t len, char* AERONET_RESTRICT to) {
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

#ifdef AERONET_HAS_ASCII_LOWER_MASK4
#undef AERONET_HAS_ASCII_LOWER_MASK4
#endif

}  // namespace aeronet
