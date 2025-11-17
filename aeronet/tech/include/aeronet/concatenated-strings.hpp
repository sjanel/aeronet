#pragma once

#include <cstdint>

#include "aeronet/dynamic-concatenated-strings.hpp"

namespace aeronet {

namespace detail {
inline constexpr char kNullCharSep[] = {'\0'};

}  // namespace detail

using ConcatenatedStrings = DynamicConcatenatedStrings<detail::kNullCharSep, false, uint64_t>;
using SmallConcatenatedStrings = DynamicConcatenatedStrings<detail::kNullCharSep, false, uint32_t>;

using SmallConcatenatedStringsCaseInsensitive = DynamicConcatenatedStrings<detail::kNullCharSep, true, uint32_t>;

}  // namespace aeronet