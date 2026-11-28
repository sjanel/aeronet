#pragma once

#include <limits>
#include <utility>

namespace aeronet {

// Casts an integral value to ToT, clamping it to the range of ToT instead of truncating it.
// For instance a length passed to a system call taking a narrower type, which then processes fewer bytes.
// This is C++26 std::saturate_cast (<numeric>, P0543), to be replaced by it once the project requires C++26.
// std::clamp cannot do it: its value and bounds share one type, and no standard type holds the ranges of both
// int64_t and uint64_t. std::cmp_greater / std::cmp_less compare mixed signedness values exactly instead.
template <class ToT, class FromT>
constexpr ToT SaturateCast(FromT value) noexcept {
  if (std::cmp_greater(value, std::numeric_limits<ToT>::max())) {
    return std::numeric_limits<ToT>::max();
  }
  if (std::cmp_less(value, std::numeric_limits<ToT>::min())) {
    return std::numeric_limits<ToT>::min();
  }
  return static_cast<ToT>(value);
}

}  // namespace aeronet