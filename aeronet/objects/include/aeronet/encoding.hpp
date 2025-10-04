#pragma once

#include <cstdint>
#include <type_traits>
#include <utility>

#include "aeronet/http-constants.hpp"

namespace aeronet {

// Ordered for aeronet preferred to least preferred as a default if no config preference is set.
enum class Encoding : std::uint8_t {
  zstd,
  gzip,
  deflate,
  br,
  none,
};

inline constexpr std::underlying_type_t<Encoding> kNbContentEncodings = 5;

constexpr std::string_view GetEncodingStr(Encoding compressionFormat) {
  switch (compressionFormat) {
    case Encoding::none:
      return http::identity;
    case Encoding::gzip:
      return http::gzip;
    case Encoding::deflate:
      return http::deflate;
    case Encoding::br:
      return http::br;
    case Encoding::zstd:
      return http::zstd;
    default:
      std::unreachable();
  }
}

}  // namespace aeronet