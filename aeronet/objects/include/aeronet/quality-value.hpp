#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace aeronet::http {

// A quality value or weight (RFC 9110 §12.4.2) in thousandths: from 0 ("not acceptable") to kQualityValueMax (1, the
// default weight).
using QualityValue = std::uint16_t;

inline constexpr QualityValue kQualityValueMax = 1000;

// Returned by TryParseQualityValue for a value that is not a qvalue.
inline constexpr QualityValue kInvalidQualityValue = std::numeric_limits<QualityValue>::max();

// Parses the value of a 'q' parameter, from just after its '=': "0" or "1" with up to 3 decimals. Anything from the
// next ' ', '\t' or ';' is ignored. An empty or invalid value (above 1, more than 3 decimals, a sign, an exponent,
// "inf", leading whitespace...) is kInvalidQualityValue.
[[nodiscard]] QualityValue TryParseQualityValue(std::string_view value) noexcept;

// Same as TryParseQualityValue, an invalid value being 0.
[[nodiscard]] inline QualityValue ParseQualityValue(std::string_view value) noexcept {
  const QualityValue quality = TryParseQualityValue(value);
  return quality == kInvalidQualityValue ? QualityValue{0} : quality;
}

}  // namespace aeronet::http
