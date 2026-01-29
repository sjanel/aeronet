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

// Bitmap type for tracking Accept-Encoding header values from client request.
// Each bit corresponds to an Encoding enum value.
using EncodingBmp = std::uint8_t;

// Convert Encoding enum to its bitmap representation.
constexpr EncodingBmp EncodingToBmp(Encoding enc) noexcept {
  return static_cast<EncodingBmp>(1U << static_cast<std::underlying_type_t<Encoding>>(enc));
}

// Check if a specific encoding is present in the bitmap.
constexpr bool EncodingBmpContains(EncodingBmp bmp, Encoding enc) noexcept { return (bmp & EncodingToBmp(enc)) != 0; }

// Parse Accept-Encoding header value into a bitmap of accepted encodings.
// This is a simplified parser that checks presence of encoding names (case-insensitive).
// Encodings with q=0 are excluded. Wildcard '*' matches all supported encodings.
// Returns a bitmap where each bit represents whether the corresponding encoding is accepted.
EncodingBmp ParseAcceptEncodingToBmp(std::string_view acceptEncoding);

}  // namespace aeronet