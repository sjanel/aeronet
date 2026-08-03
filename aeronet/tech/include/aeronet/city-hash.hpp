#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/internal/city.hpp"

namespace aeronet {

struct CityHash {
  std::size_t operator()(std::string_view str) const noexcept {
    if constexpr (sizeof(std::size_t) == 4) {
      return static_cast<std::size_t>(City::CityHash32(str.data(), str.size()));
    } else {
      return static_cast<std::size_t>(City::CityHash64(str.data(), str.size()));
    }
  }
};

}  // namespace aeronet