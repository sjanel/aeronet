#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/internal/city.hpp"

namespace aeronet {

struct CityHash {
  std::size_t operator()(std::string_view str) const noexcept {
    return static_cast<std::size_t>(City::CityHash64(str.data(), str.size()));
  }
};

}  // namespace aeronet