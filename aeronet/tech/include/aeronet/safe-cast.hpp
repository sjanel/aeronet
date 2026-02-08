#pragma once

#include <limits>
#include <stdexcept>
#include <type_traits>

namespace aeronet {

template <class ToT, class FromT>
constexpr ToT SafeCast(FromT value) {
  // Reject negative values when casting to an unsigned type
  if constexpr (std::is_signed_v<FromT> && std::is_unsigned_v<ToT>) {
    if (value < 0) [[unlikely]] {
      throw std::overflow_error("negative value cannot be represented in unsigned target type");
    }
  }
  if constexpr (sizeof(ToT) < sizeof(FromT) ||
                (sizeof(ToT) == sizeof(FromT) && std::is_unsigned_v<ToT> && std::is_signed_v<FromT>)) {
    if (static_cast<std::make_unsigned_t<FromT>>(value) > std::numeric_limits<ToT>::max()) [[unlikely]] {
      throw std::overflow_error("value exceeds target type maximum");
    }
  }
  return static_cast<ToT>(value);
}

}  // namespace aeronet