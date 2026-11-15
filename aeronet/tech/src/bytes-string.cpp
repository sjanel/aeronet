#include "aeronet/bytes-string.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"

namespace aeronet {

namespace {

constexpr std::string_view kUnits[]{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};

constexpr std::uint32_t kNoDecimal = 10U;

// The displayed value of a size: "intPart unit", or "intPart.decimal unit" when 'decimal' is not kNoDecimal.
struct DisplayedSize {
  std::uintmax_t intPart;
  std::uint32_t decimal;
  std::uint32_t unitIdx;
};

DisplayedSize ComputeDisplayedSize(std::uintmax_t size) {
  // The largest unit in which the value is at least 1: a unit is 10 bits, and the 64 bits of a size end in EiB.
  const auto unitIdx = static_cast<std::uint32_t>((static_cast<unsigned>(std::bit_width(size | 1U)) - 1U) / 10U);
  if (unitIdx == 0U) {
    return {size, kNoDecimal, 0U};
  }
  const std::uint32_t shift = 10U * unitIdx;
  const std::uintmax_t intPart = size >> shift;
  if (intPart >= 10U) {
    // No decimal, rounded half up.
    return {intPart + ((size >> (shift - 1U)) & 1U), kNoDecimal, unitIdx};
  }
  // One decimal, rounded half up. The remainder is below 2^60: 10 times it plus a half unit fits 64 bits.
  const std::uintmax_t rem = size & ((std::uintmax_t{1} << shift) - 1U);
  const auto decimal = static_cast<std::uint32_t>(((rem * 10U) + (std::uintmax_t{1} << (shift - 1U))) >> shift);
  if (decimal == 10U) {
    // Carry into the integer part: 1.96 is displayed "2.0", 9.96 "10" (no decimal from 10).
    return {intPart + 1U, intPart + 1U == 10U ? kNoDecimal : 0U, unitIdx};
  }
  return {intPart, decimal, unitIdx};
}

}  // namespace

char* WriteFormattedSize(std::uintmax_t size, char* out) {
  const DisplayedSize displayed = ComputeDisplayedSize(size);
  out = WriteUInt(out, displayed.intPart, ndigits(displayed.intPart));
  if (displayed.decimal != kNoDecimal) {
    *out++ = '.';
    *out++ = static_cast<char>('0' + displayed.decimal);
  }
  *out++ = ' ';
  return Append(kUnits[displayed.unitIdx], out);
}

std::size_t FormattedSizeLen(std::uintmax_t size) {
  const DisplayedSize displayed = ComputeDisplayedSize(size);
  return ndigits(displayed.intPart) + (displayed.decimal == kNoDecimal ? 0U : 2U) + 1U +
         kUnits[displayed.unitIdx].size();
}

}  // namespace aeronet
