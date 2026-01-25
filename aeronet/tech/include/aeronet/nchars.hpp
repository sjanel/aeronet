#pragma once

#include <concepts>
#include <cstdint>

#include "aeronet/ndigits.hpp"

namespace aeronet {

/// Count the number of digits including the possible minus sign for negative integrals.
constexpr std::uint8_t nchars(std::signed_integral auto n) noexcept {
  return static_cast<std::uint8_t>(ndigits(n) + (n < 0));
}

/// Synonym of ndigits for unsigned types.
constexpr std::uint8_t nchars(std::unsigned_integral auto n) noexcept { return ndigits(n); }

}  // namespace aeronet