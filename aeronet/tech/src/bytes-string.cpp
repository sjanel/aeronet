#include "aeronet/bytes-string.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "aeronet/raw-chars.hpp"
#include "aeronet/stringconv.hpp"

namespace aeronet {

void AddFormattedSize(std::uintmax_t size, RawChars& out) {
  static constexpr std::string_view units[]{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};

  // Find the largest unit where the value is < 1024 (binary units, 1024^n)
  std::size_t unitIdx = 0;
  std::uintmax_t divisor = 1;
  // Grow divisor but avoid overflow: use division to check whether next multiply would exceed size.
  for (; unitIdx + 1U < std::size(units) && divisor <= size / 1024ULL; ++unitIdx) {
    divisor *= 1024ULL;
  }

  // small helper: append integer value and the unit (with leading space)
  const auto appendIntAndUnit = [&out](std::uintmax_t value, std::string_view unit) {
    const auto buf = IntegralToCharVector(value);
    out.ensureAvailableCapacityExponential(buf.size() + 1 + unit.size());
    out.unchecked_append(buf.data(), buf.size());
    out.unchecked_push_back(' ');
    out.unchecked_append(unit);
  };

  // Bytes: print integer bytes
  if (unitIdx == 0U) {
    appendIntAndUnit(size, units[unitIdx]);
    return;
  }

  // For units >= KB, follow existing formatting rules: if the numeric value is < 10, print one
  // decimal place (rounded). Otherwise print an integer (rounded).
  // For typical sizes where size <= max/10 we can use the original fast path which uses
  // integer arithmetic without overflow. For very large sizes (size > max/10) use a
  // separate branch that avoids multiplications that could overflow.
  static constexpr std::uintmax_t kMaxDiv10 = std::numeric_limits<std::uintmax_t>::max() / 10U;

  if (size < divisor * 10U) {
    const std::uintmax_t intPart = size / divisor;
    const std::uintmax_t rem = size % divisor;
    // frac10 = round(rem * 10 / divisor)
    const std::uintmax_t frac10 = (rem * 10U + divisor / 2U) / divisor;
    std::uintmax_t finalInt = intPart;
    std::uintmax_t finalFrac = frac10;
    if (frac10 >= 10U) {
      // carry into integer part (e.g. 9.96 -> rounds to 10.0)
      finalInt = intPart + 1U;
      finalFrac = 0U;
    }
    // If carry produced a value >= 10 we should print integer form (no decimal) per rules
    if (finalInt >= 10U) {
      appendIntAndUnit(finalInt, units[unitIdx]);
      return;
    }
    // print one decimal: int.frac unit
    const auto intBuf = IntegralToCharVector(finalInt);
    const auto fracBuf = IntegralToCharVector(finalFrac);
    out.ensureAvailableCapacityExponential(intBuf.size() + 1U + fracBuf.size() + 1U + units[unitIdx].size());
    out.unchecked_append(intBuf.data(), intBuf.size());
    out.unchecked_push_back('.');
    out.unchecked_append(fracBuf.data(), fracBuf.size());
    out.unchecked_push_back(' ');
    out.unchecked_append(units[unitIdx]);
    return;
  }

  if (size <= kMaxDiv10) {
    // Print integer with rounding (safe because size <= max/10)
    const std::uintmax_t rounded = (size + divisor / 2U) / divisor;
    appendIntAndUnit(rounded, units[unitIdx]);
  } else {
    // Integer rounding for large values: safe because we avoid adding divisor to size.
    std::uintmax_t rounded = size / divisor;
    if (size % divisor >= divisor / 2U) {
      ++rounded;
    }
    appendIntAndUnit(rounded, units[unitIdx]);
  }
}

}  // namespace aeronet