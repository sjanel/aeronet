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

  if constexpr (sizeof(n) <= sizeof(std::uint16_t)) {
    static constexpr uint16_t kThresholds[]{100, 1000, 10000};
    uint8_t digits = 2;
    digits = static_cast<uint8_t>(digits + (n >= kThresholds[0]));
    digits = static_cast<uint8_t>(digits + (n >= kThresholds[1]));
    digits = static_cast<uint8_t>(digits + (n >= kThresholds[2]));
    return digits;
  } else {
    using U = std::uintmax_t;

    static constexpr uint8_t kMaxNbBits = (sizeof(U) * 8U) - 1U;
    static constexpr auto pow10 = [] {
      std::array<U, std::numeric_limits<U>::digits10 + 1> arr{};
      arr[0] = 9U;
      for (unsigned int i = 1U; i < std::numeric_limits<U>::digits10; ++i) {
        arr[i] = (arr[i - 1U] * 10U) + 9U;
      }
      arr[std::numeric_limits<U>::digits10] = std::numeric_limits<U>::max();
      return arr;
    }();

    const uint8_t floorLog2 = kMaxNbBits - static_cast<uint8_t>(std::countl_zero(static_cast<U>(n)));

    uint8_t estimate = static_cast<uint8_t>((1233U * floorLog2) >> 12U);

    static_assert(((1233U * kMaxNbBits) >> 12U) <= std::numeric_limits<U>::digits10,
                  "ndigits approximation bound changed");

    return static_cast<uint8_t>(estimate + 1U + (pow10[estimate] < n));
  }
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