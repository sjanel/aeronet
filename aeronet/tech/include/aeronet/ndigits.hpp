#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace aeronet {

// Fast unsigned implementation using countl_zero to estimate floor(log2(n)),
// then map to decimal digits via a small correction against powers of 10.
constexpr std::uint8_t ndigits(std::unsigned_integral auto n) noexcept {
  if (n < 10U) {
    return 1U;
  }

  // Pin the largest unsigned type
  using U = std::uintmax_t;

  // Powers of 10 - 1 up to the maximum of U
  static constexpr unsigned int kMaxNbBits = (sizeof(U) * 8U) - 1U;
  static constexpr auto pow10 = []() {
    std::array<U, std::numeric_limits<U>::digits10 + 1> arr{};
    arr[0] = 9U;
    for (unsigned int i = 1U; i < std::numeric_limits<U>::digits10; ++i) {
      arr[i] = (arr[i - 1U] * 10U) + 9U;
    }
    arr[std::numeric_limits<U>::digits10] = std::numeric_limits<U>::max();
    return arr;
  }();

  // We need to cast to uintmax_t otherwise it would return the number of leading zeros
  // in the type of n, which can be smaller than uintmax_t.
  const unsigned int floorLog2 = kMaxNbBits - static_cast<unsigned int>(std::countl_zero(static_cast<U>(n)));

  // estimate decimal digits-1 using fixed-point approximation of log10(2)
  // multiply by 1233/4096 ~= log10(2)
  unsigned int estimate = (floorLog2 * 1233U) >> 12U;

  // The fixed-point approximation (1233/4096) guarantees the initial estimate
  // never exceeds `digits10` (it may reach `digits10` for wider types).
  // Sanity-check at compile-time so future edits (or larger integral types)
  // won't break the invariant.
  static_assert(((kMaxNbBits * 1233U) >> 12U) <= std::numeric_limits<U>::digits10,
                "ndigits approximation bound changed");

  // The initial estimate may occasionally be low by at most one
  if (pow10[estimate] < n) {
    ++estimate;
  }

  return static_cast<std::uint8_t>(estimate + 1U);
}

// Signed wrapper: compute absolute value safely in unsigned type to avoid
// overflow for minimum value and delegate to unsigned implementation.
constexpr std::uint8_t ndigits(std::signed_integral auto val) noexcept {
  using T = decltype(val);
  using U = std::make_unsigned_t<T>;

  if (val >= 0) {
    return ndigits(static_cast<U>(val));
  }

  // Use -(val+1) which is representable, then add 1 after casting to unsigned.
  return ndigits(static_cast<U>(-(val + 1)) + 1U);
}

}  // namespace aeronet