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
  static constexpr std::string_view kEncodingStrs[]{
      http::zstd, http::br, http::gzip, http::deflate, http::identity,
  };
  static_assert(std::size(kEncodingStrs) == kNbContentEncodings);
  return kEncodingStrs[static_cast<std::underlying_type_t<Encoding>>(enc)];
}

// Check if encoding is enabled in this build.
constexpr bool IsEncodingEnabled(Encoding enc) {
  static constexpr bool kEncodingEnabled[]{
      zstdEnabled(), brotliEnabled(), zlibEnabled(), zlibEnabled(), true,
  };
  static_assert(std::size(kEncodingEnabled) == kNbContentEncodings);
  return kEncodingEnabled[static_cast<std::underlying_type_t<Encoding>>(enc)];
}

// Number of encodings enabled in this build, Encoding::none included.
inline constexpr std::underlying_type_t<Encoding> kNbSupportedEncodings = [] {
  std::underlying_type_t<Encoding> count = 0;
  for (std::underlying_type_t<Encoding> pos = 0; pos < kNbContentEncodings; ++pos) {
    if (IsEncodingEnabled(static_cast<Encoding>(pos))) {
      ++count;
    }
  }
  return count;
}();

}  // namespace aeronet
