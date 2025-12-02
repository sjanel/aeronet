#pragma once

#include <cstdint>

namespace aeronet {

/// Optimization of ipow(10, uint8_t exp)
/// Returns uint64_t power of 10 for exponents 0-19, or max value for larger exponents.
constexpr uint64_t ipow10(uint32_t exp) noexcept {
  constexpr uint64_t kPow10Table[] = {1ULL,
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
  return exp < sizeof(kPow10Table) / sizeof(kPow10Table[0]) ? kPow10Table[exp] : kPow10Table[18];
}

}  // namespace aeronet
