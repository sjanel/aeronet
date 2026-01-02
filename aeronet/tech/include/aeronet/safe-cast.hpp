#pragma once

#include <limits>
#include <stdexcept>

namespace aeronet {

template <class ToT, class FromT>
constexpr ToT SafeCast(FromT value) {
  if constexpr (sizeof(ToT) < sizeof(FromT)) {
    if (std::numeric_limits<ToT>::max() < value) [[unlikely]] {
      throw std::overflow_error("value exceeds target type maximum");
    }
  }
  return static_cast<ToT>(value);
}

}  // namespace aeronet