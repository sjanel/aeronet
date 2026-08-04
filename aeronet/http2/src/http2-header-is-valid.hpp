#pragma once

#include <algorithm>
#include <string_view>

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
// NUL, CR, or LF. Everything else — including bytes >= 0x80 - is legal opaque data;
// this is exactly what the HPACK Huffman code (RFC 7541 Appendix B, which has a code
// for all 256 byte values) is designed to transport. This is intentionally more permissive
// than aeronet::http::IsValidHeaderValue, which restricts to printable ASCII - a separate
// and defensible choice for HTTP/1.1, but not required or enforced by HTTP/2.
constexpr bool IsValidHTTP2HeaderValue(std::string_view value) noexcept {
  struct AllowedChars {
    bool res[256]{};
  };
  static constexpr auto kAllowed = [] {
    AllowedChars allowed{};
    for (std::size_t ch = 0; ch < 256; ++ch) {
      allowed.res[static_cast<unsigned char>(ch)] = ch != '\0' && ch != '\r' && ch != '\n';
    }
    return allowed;
  }();

  return std::ranges::all_of(value, [](char ch) { return kAllowed.res[static_cast<unsigned char>(ch)]; });
}

}  // namespace aeronet::http2