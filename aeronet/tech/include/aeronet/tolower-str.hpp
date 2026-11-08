#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "aeronet/ascii-lower-mask.hpp"
#include "aeronet/compiler-config.hpp"
#include "aeronet/toupperlower.hpp"

#ifdef AERONET_HAS_ASCII_LOWER_MASK4
#include "aeronet/avx2.hpp"
#endif

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
  if (HasAvx2()) {
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
  if consteval {
    for (std::size_t pos = 0; pos < len; ++pos) {
      to[pos] = tolower(from[pos]);
    }
    return;
  }

  // 8 bytes at a time, whatever the alignment of the buffers. std::copy_n and not std::memcpy for the store: with
  // _FORTIFY_SOURCE (on by default with Ubuntu's GCC), a memcpy into a stack buffer stays a library call once inlined.
  for (std::size_t nbWords = len / sizeof(uint64_t); nbWords != 0; --nbWords) {
    uint64_t val;
    std::memcpy(&val, from, sizeof(val));
    val = AsciiLowerMask(val);
    std::copy_n(reinterpret_cast<const char*>(&val), sizeof(val), to);
    from += sizeof(val);
    to += sizeof(val);
  }

  // A computed count, for GCC to know that it is below 8: otherwise it vectorizes this loop, and warns about the
  // 16-byte stores of that dead code when the destination is a small array.
  const std::size_t tailLen = len % sizeof(uint64_t);
  for (std::size_t pos = 0; pos < tailLen; ++pos) {
    to[pos] = tolower(from[pos]);
  }
}

#ifdef AERONET_HAS_ASCII_LOWER_MASK4
#undef AERONET_HAS_ASCII_LOWER_MASK4
#endif

}  // namespace aeronet
