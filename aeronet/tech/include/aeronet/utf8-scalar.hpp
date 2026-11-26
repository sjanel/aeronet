#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aeronet {

/// Validates a single multibyte UTF-8 sequence.
/// @param ptr  Points to 1 byte past the lead byte on entry; advanced past the full sequence on success.
/// @param end  One-past-end of the buffer.
/// @param lead The leading byte of the sequence (already consumed), at least 0x80.
/// @return true if the sequence is valid: no overlong encoding, no surrogate (U+D800 - U+DFFF), no code point above
/// U+10FFFF and no truncated sequence.
constexpr bool ValidateUtf8MultibyteSequence(const uint8_t*& ptr, const uint8_t* end, uint8_t lead) noexcept {
  uint32_t remaining;
  uint32_t codepoint;
  uint32_t minCodepoint;

  if ((lead & 0xE0U) == 0xC0U) {
    remaining = 1U;
    codepoint = lead & 0x1FU;
    minCodepoint = 0x80U;
  } else if ((lead & 0xF0U) == 0xE0U) {
    remaining = 2U;
    codepoint = lead & 0x0FU;
    minCodepoint = 0x800U;
  } else if ((lead & 0xF8U) == 0xF0U) {
    remaining = 3U;
    codepoint = lead & 0x07U;
    minCodepoint = 0x10000U;
  } else {
    return false;
  }

  if (end - ptr < static_cast<std::ptrdiff_t>(remaining)) {
    return false;
  }

  for (decltype(remaining) idx = 0; idx < remaining; ++idx) {
    const uint8_t byte = *ptr++;
    if ((byte & 0xC0U) != 0x80U) {
      return false;
    }
    codepoint = (codepoint << 6U) | (byte & 0x3FU);
  }

  if (codepoint < minCodepoint) {
    return false;
  }
  if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
    return false;
  }
  return codepoint <= 0x10FFFFU;
}

/// Tells whether [ptr, end) is well-formed UTF-8 (RFC 3629, see ValidateUtf8MultibyteSequence()), with a scalar loop
/// skipping 8 ASCII bytes at a time. Fast enough for short strings; aeronet::websocket::IsValidUtf8() runs SIMD
/// kernels for long ones.
inline bool IsValidUtf8Scalar(const uint8_t* ptr, const uint8_t* end) noexcept {
  static constexpr uint64_t kHighBits = 0x8080808080808080ULL;

  while (ptr != end) {
    if (end - ptr >= static_cast<std::ptrdiff_t>(sizeof(uint64_t))) {
      uint64_t word;
      std::memcpy(&word, ptr, sizeof(word));
      if ((word & kHighBits) == 0) {
        ptr += sizeof(word);
        continue;
      }
    }
    const uint8_t byte = *ptr++;
    if (byte >= 0x80 && !ValidateUtf8MultibyteSequence(ptr, end, byte)) {
      return false;
    }
  }
  return true;
}

}  // namespace aeronet
