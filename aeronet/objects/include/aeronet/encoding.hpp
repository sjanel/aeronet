#pragma once

#include <cstdint>
#include <utility>

#include "aeronet/http-constants.hpp"

namespace aeronet {

// Ordered for aeronet preferred to least preferred as a default if no config preference is set.
enum class Encoding : std::uint8_t {
  zstd,
  gzip,
  deflate,
  none,
};

inline constexpr std::uint8_t kNbContentEncodings = 4;

constexpr std::string_view GetEncodingStr(Encoding compressionFormat) {
  switch (compressionFormat) {
    case Encoding::none:
      return http::identity;
    case Encoding::gzip:
      return http::gzip;
    case Encoding::deflate:
      return http::deflate;
    case Encoding::zstd:
      return http::zstd;
    default:
      std::unreachable();
  }
}

}  // namespace aeronet