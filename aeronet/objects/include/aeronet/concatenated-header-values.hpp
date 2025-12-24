#pragma once

#include <cstdint>

#include "aeronet/dynamic-concatenated-strings.hpp"

namespace aeronet {

namespace detail {
inline constexpr const char kHeaderValueSep[] = ", ";
}

using ConcatenatedHeaderValues = DynamicConcatenatedStrings<detail::kHeaderValueSep, uint32_t>;

}  // namespace aeronet