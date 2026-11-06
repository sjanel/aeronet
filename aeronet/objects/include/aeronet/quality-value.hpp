#pragma once

#include <cstdint>
#include <string_view>

namespace aeronet::http {

// A quality value or weight (RFC 9110 §12.4.2) in thousandths: from 0 ("not acceptable") to kQualityValueMax (1, the
// default weight).
using QualityValue = std::uint16_t;

inline constexpr QualityValue kQualityValueMax = 1000;

// Parses the value of a 'q' parameter, from just after its '=': "0" or "1" with up to 3 decimals. Anything from the
// next ' ', '\t' or ';' is ignored. An empty or invalid value (above 1, more than 3 decimals, a sign, an exponent,
// "inf", leading whitespace...) is 0.
[[nodiscard]] QualityValue ParseQualityValue(std::string_view value) noexcept;

}  // namespace aeronet::http
