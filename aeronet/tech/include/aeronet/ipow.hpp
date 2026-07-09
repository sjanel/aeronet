#pragma once

#include <cstdint>

namespace aeronet {

/// Computes the power of 10 for a given exponent.
/// Returns uint64_t power of 10 for exponents 0-19, or max value for larger exponents.
constexpr uint64_t ipow10(uint32_t exp) noexcept {
  static constexpr uint64_t kPow10Table[] = {1ULL,
                                             10ULL,
                                             100ULL,
                                             1000ULL,
                                             10000ULL,
                                             100000ULL,
                                             1000000ULL,
                                             10000000ULL,
                                             100000000ULL,
                                             1000000000ULL,
                                             10000000000ULL,
                                             100000000000ULL,
                                             1000000000000ULL,
                                             10000000000000ULL,
                                             100000000000000ULL,
                                             1000000000000000ULL,
                                             10000000000000000ULL,
                                             100000000000000000ULL,
                                             1000000000000000000ULL,
                                             10000000000000000000ULL};
  static constexpr uint32_t kCount = sizeof(kPow10Table) / sizeof(kPow10Table[0]);
  return kPow10Table[exp < kCount ? exp : kCount - 1];
}

}  // namespace aeronet
