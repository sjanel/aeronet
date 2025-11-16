#pragma once

#include <cstdint>

#include "aeronet/dynamic-concatenated-strings.hpp"

namespace aeronet {

namespace detail {
inline constexpr char kCRLFChars[] = {"\r\n"};
}

using ConcatenatedHeaders = DynamicConcatenatedStrings<detail::kCRLFChars, false, uint32_t>;

}  // namespace aeronet