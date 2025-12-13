#pragma once

#include <bit>
#include <concepts>
#include <type_traits>

namespace aeronet {

// Fast unsigned implementation using countl_zero to estimate floor(log2(n)),
// then map to decimal digits via a small correction against powers of 10.
constexpr int ndigits(std::unsigned_integral auto n) noexcept {
  if (n < 10U) {
    return 1;
  }

  // Powers of 10 up to 10^19
  static constexpr unsigned long long pow10[] = {9ULL,
                                                 99ULL,
                                                 999ULL,
                                                 9999ULL,
                                                 99999ULL,
                                                 999999ULL,
                                                 9999999ULL,
                                                 99999999ULL,
                                                 999999999ULL,
                                                 9999999999ULL,
                                                 99999999999ULL,
                                                 999999999999ULL,
                                                 9999999999999ULL,
                                                 99999999999999ULL,
                                                 999999999999999ULL,
                                                 9999999999999999ULL,
                                                 99999999999999999ULL,
                                                 999999999999999999ULL,
                                                 9999999999999999999ULL,
                                                 static_cast<unsigned long long>(-1)};

  // Promote to 64-bit for countl_zero to avoid promotion surprises on narrow types.
  const unsigned long long u64 = static_cast<unsigned long long>(n);
  const unsigned int floorLog2 = 63U - static_cast<unsigned int>(std::countl_zero(u64));

  // estimate decimal digits-1 using fixed-point approximation of log10(2)
  // multiply by 1233/4096 ~= log10(2)
  unsigned int estimate = (floorLog2 * 1233U) >> 12U;

  // The fixed-point approximation (1233/4096) guarantees the initial
  // estimate is never more than 18 for 64-bit inputs. Sanity-check that at
  // compile-time so future edits won't break the invariant.
  static_assert(((63U * 1233U) >> 12U) == 18U, "ndigits approximation bound changed");

  // The initial estimate may occasionally be low by at most one
  if (u64 > pow10[estimate]) {
    ++estimate;
  }

  return static_cast<int>(estimate) + 1;
}

// Signed wrapper: compute absolute value safely in unsigned type to avoid
// overflow for minimum value and delegate to unsigned implementation.
constexpr int ndigits(std::signed_integral auto val) noexcept {
  using T = decltype(val);
  using U = std::make_unsigned_t<T>;

  if (val >= 0) {
    return ndigits(static_cast<U>(val));
  }

  // Use -(val+1) which is representable, then add 1 after casting to unsigned.
  return ndigits(static_cast<U>(-(val + 1)) + 1U);
}

}  // namespace aeronet