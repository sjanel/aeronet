#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace aeronet {

// Cold paths of SafeCast: log the offending value and the maximum of the target type, then throw std::overflow_error.
// They are defined out of line (safe-cast.cpp) to keep the exception machinery out of every SafeCast instantiation:
// the cast stays a compare and a branch, with no prologue on the success path.
[[noreturn]] void ThrowSafeCastNegative(std::intmax_t value);
[[noreturn]] void ThrowSafeCastOverflow(std::intmax_t value, std::uintmax_t maxValue);
[[noreturn]] void ThrowSafeCastOverflow(std::uintmax_t value, std::uintmax_t maxValue);

// Casts an integral value to ToT, throws std::overflow_error if it cannot be represented.
template <class ToT, class FromT>
constexpr ToT SafeCast(FromT value) {
  // Reject negative values when casting to an unsigned type
  if constexpr (std::is_signed_v<FromT> && std::is_unsigned_v<ToT>) {
    if (value < 0) [[unlikely]] {
      ThrowSafeCastNegative(static_cast<std::intmax_t>(value));
    }
  }
  if constexpr (sizeof(ToT) < sizeof(FromT) ||
                (sizeof(ToT) == sizeof(FromT) && std::is_signed_v<FromT> && std::is_unsigned_v<ToT>)) {
    if (static_cast<std::make_unsigned_t<ToT>>(std::numeric_limits<ToT>::max()) <
        static_cast<std::make_unsigned_t<FromT>>(value)) [[unlikely]] {
      constexpr auto kMaxValue = static_cast<std::uintmax_t>(std::numeric_limits<ToT>::max());
      if constexpr (std::is_signed_v<FromT>) {
        ThrowSafeCastOverflow(static_cast<std::intmax_t>(value), kMaxValue);
      } else {
        ThrowSafeCastOverflow(static_cast<std::uintmax_t>(value), kMaxValue);
      }
    }
  }
  return static_cast<ToT>(value);
}

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