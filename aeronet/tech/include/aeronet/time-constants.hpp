#pragma once

#include <cstddef>

#include "aeronet/timedef.hpp"

namespace aeronet {

inline constexpr std::size_t ISO8601UTCWithMsStrLen = 24;  // "YYYY-MM-DDThh:mm:ss.sssZ"
inline constexpr std::size_t RFC7231DateStrLen = 29;       // "Sun, 23 Aug 2026 18:51:26 GMT"
inline constexpr SysTimePoint kInvalidTimePoint = SysTimePoint::max();

}  // namespace aeronet