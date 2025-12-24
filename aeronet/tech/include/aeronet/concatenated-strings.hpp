#pragma once

#include <cstdint>

#include "aeronet/dynamic-concatenated-strings.hpp"

namespace aeronet {

namespace detail {
inline constexpr char kNullCharSep[] = {'\0'};

}  // namespace detail

using ConcatenatedStrings = DynamicConcatenatedStrings<detail::kNullCharSep, uint64_t>;
using SmallConcatenatedStrings = DynamicConcatenatedStrings<detail::kNullCharSep, uint32_t>;

}  // namespace aeronet