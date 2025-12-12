#pragma once

#include <cstdint>
#include <type_traits>

#include "aeronet/http-constants.hpp"

namespace aeronet {

// Ordered for aeronet preferred to least preferred as a default if no config preference is set.
enum class Encoding : std::uint8_t {
  zstd,
  br,
  gzip,
  deflate,
  none,
};

inline constexpr std::underlying_type_t<Encoding> kNbContentEncodings = 5;

constexpr std::string_view GetEncodingStr(Encoding compressionFormat) {
  static constexpr std::string_view kEncodingStrs[kNbContentEncodings] = {
      http::zstd, http::br, http::gzip, http::deflate, http::identity,
  };
  return kEncodingStrs[static_cast<std::underlying_type_t<Encoding>>(compressionFormat)];
}

}  // namespace aeronet