#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "aeronet/features.hpp"
#include "aeronet/http-constants.hpp"

namespace aeronet {

// Ordered for aeronet preferred to least preferred as a default if no config preference is set.
enum class Encoding : std::uint8_t {
  zstd,
  br,
  gzip,
  deflate,
  none,  // should be last
};

inline constexpr std::underlying_type_t<Encoding> kNbContentEncodings =
    static_cast<std::underlying_type_t<Encoding>>(Encoding::none) + 1;

// Get string representation of encoding for use in HTTP headers.
constexpr std::string_view GetEncodingStr(Encoding enc) {
  static constexpr std::string_view kEncodingStrs[kNbContentEncodings] = {
      http::zstd, http::br, http::gzip, http::deflate, http::identity,
  };
  if (static_cast<std::underlying_type_t<Encoding>>(enc) >= kNbContentEncodings) [[unlikely]] {
    return "unknown";
  }
  return kEncodingStrs[static_cast<std::underlying_type_t<Encoding>>(enc)];
}

// Check if encoding is enabled in this build.
constexpr bool IsEncodingEnabled(Encoding enc) {
  static constexpr bool kEncodingEnabled[kNbContentEncodings] = {
      zstdEnabled(), brotliEnabled(), zlibEnabled(), zlibEnabled(), true,
  };
  if (static_cast<std::underlying_type_t<Encoding>>(enc) >= kNbContentEncodings) [[unlikely]] {
    return false;
  }
  return kEncodingEnabled[static_cast<std::underlying_type_t<Encoding>>(enc)];
}

}  // namespace aeronet