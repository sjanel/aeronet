#pragma once

#include <cstddef>

namespace aeronet {

inline constexpr std::size_t ISO8601UTCWithMsStrLen = 24;  // "YYYY-MM-DDThh:mm:ss.sssZ"
inline constexpr std::size_t RFC7231DateStrLen = 29;

}  // namespace aeronet