#pragma once

#include <cstdint>
#include <limits>

namespace aeronet {
/// constexpr and integral version of math.power.
/// Taken from https://gist.github.com/orlp/3551590
constexpr int64_t ipow(int64_t base, uint8_t exp) noexcept {
  constexpr uint8_t kHighestBitSet[] = {0,   1,   2,   2,   3,   3,   3,   3,   4,   4,   4,   4,   4,   4,   4,
                                        4,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,   5,
                                        5,   5,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,
                                        6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,
                                        6,   6,   6,   255,  // anything past 63 is a guaranteed overflow with base > 1
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};

  int64_t result = 1;

  switch (kHighestBitSet[exp]) {
    case 255:  // we use 255 as an overflow marker and return 0 on overflow/underflow
      if (base == 1) {
        return 1;
      }
      if (base != -1) {
        return 0;
      }
      return 1 - (2 * (exp & 1));
    case 6:
      if ((exp & 1U) != 0) {
        result *= base;
      }
      exp >>= 1;
      base *= base;
      [[fallthrough]];
    case 5:
      if ((exp & 1U) != 0) {
        result *= base;
      }
      exp >>= 1;
      base *= base;
      [[fallthrough]];
    case 4:
      if ((exp & 1U) != 0) {
        result *= base;
      }
      exp >>= 1;
      base *= base;
      [[fallthrough]];
    case 3:
      if ((exp & 1U) != 0) {
        result *= base;
      }
      exp >>= 1;
      base *= base;
      [[fallthrough]];
    case 2:
      if ((exp & 1U) != 0) {
        result *= base;
      }
      exp >>= 1;
      base *= base;
      [[fallthrough]];
    case 1:
      if ((exp & 1U) != 0) {
        result *= base;
      }
      [[fallthrough]];
    default:
      return result;
  }
}

/// Optimization of ipow(10, uint8_t exp)
constexpr int64_t ipow10(uint8_t exp) noexcept {
  constexpr const int64_t kPow10Table[] = {1LL,
                                           10LL,
                                           100LL,
                                           1000LL,
                                           10000LL,
                                           100000LL,
                                           1000000LL,
                                           10000000LL,
                                           100000000LL,
                                           1000000000LL,
                                           10000000000LL,
                                           100000000000LL,
                                           1000000000000LL,
                                           10000000000000LL,
                                           100000000000000LL,
                                           1000000000000000LL,
                                           10000000000000000LL,
                                           100000000000000000LL,
                                           1000000000000000000LL};
  return exp < sizeof(kPow10Table) / sizeof(kPow10Table[0]) ? kPow10Table[exp] : std::numeric_limits<int64_t>::max();
}

}  // namespace aeronet
