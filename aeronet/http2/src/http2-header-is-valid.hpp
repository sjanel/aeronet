#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "aeronet/is-header-whitespace.hpp"
#include "aeronet/tchars.hpp"

namespace aeronet::http2 {

constexpr bool IsValidHTTP2HeaderName(std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }
  if (name.front() == ':') {
    // Pseudo-header (:method, :path, ...). Further validation is done outside this function.
    return true;
  }
  struct AllowedChars {
    bool res[256]{};
  };
  static constexpr auto kAllowed = [] {
    AllowedChars allowed{};
    for (std::size_t ch = 0; ch < 256; ++ch) {
      allowed.res[static_cast<unsigned char>(ch)] =
          is_tchar(static_cast<char>(ch)) && (static_cast<char>(ch) < 'A' || static_cast<char>(ch) > 'Z');
    }
    return allowed;
  }();

  return std::ranges::all_of(name, [](char ch) { return kAllowed.res[static_cast<unsigned char>(ch)]; });
}

// RFC 9113 §8.2.1 : A HTTP/2 header field value is invalid only if it contains
// NUL, CR, or LF, or if it starts or ends with whitespace (SP or HTAB) - such a value would
// not survive a translation to HTTP/1.1, where surrounding whitespace is not part of the value.
// Everything else - including bytes >= 0x80 - is legal opaque data;
// this is exactly what the HPACK Huffman code (RFC 7541 Appendix B, which has a code
// for all 256 byte values) is designed to transport. This is intentionally more permissive
// than aeronet::http::IsValidReceivedHeaderValue, which also rejects the other control characters
// (RFC 9110 §5.5 field-value grammar) - a stricter choice for HTTP/1.1, not enforced by HTTP/2.
constexpr bool IsValidHTTP2HeaderValue(std::string_view value) noexcept {
  if (value.empty()) {
    return true;
  }
  if (http::IsHeaderWhitespace(value.front()) || http::IsHeaderWhitespace(value.back())) {
    return false;
  }
  const auto isAllowed = [](char ch) { return ch != '\0' && ch != '\r' && ch != '\n'; };
  if consteval {
    return std::ranges::all_of(value, isAllowed);
  }
  if (value.size() < sizeof(uint32_t)) {
    return std::ranges::all_of(value, isAllowed);
  }
  // NUL, CR and LF are all below 0x0E: testing 8 bytes at a time that none is below 0x0E settles almost every value.
  // The test is exact for the word as a whole: a lane is wrongly marked only by the borrow of a lower lane below 0x0E.
  // The rare value with such a byte (an HTAB, which is allowed) is checked again byte per byte.
  static constexpr uint64_t kLoBits = 0x0101010101010101ULL;
  static constexpr uint64_t kHiBits = 0x8080808080808080ULL;
  const auto bytesBelow0E = [](uint64_t word) { return (word - (kLoBits * 0x0EU)) & ~word & kHiBits; };
  const char* pos = value.data();
  const char* const end = pos + value.size();
  uint64_t below = 0;
  uint64_t word;
  if (value.size() >= sizeof(word)) {
    for (; end - pos > static_cast<std::ptrdiff_t>(sizeof(word)); pos += sizeof(word)) {
      std::memcpy(&word, pos, sizeof(word));
      below |= bytesBelow0E(word);
    }
    // The last word overlaps the previous one.
    std::memcpy(&word, end - sizeof(word), sizeof(word));
  } else {
    // 4 to 7 bytes: two overlapping halves.
    uint32_t low;
    uint32_t high;
    std::memcpy(&low, pos, sizeof(low));
    std::memcpy(&high, end - sizeof(high), sizeof(high));
    word = low | (static_cast<uint64_t>(high) << 32U);
  }
  below |= bytesBelow0E(word);
  return below == 0 || std::ranges::all_of(value, isAllowed);
}

}  // namespace aeronet::http2