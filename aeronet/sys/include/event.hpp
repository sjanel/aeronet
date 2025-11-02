#pragma once

#include <cstdint>

namespace aeronet {

using EventBmp = uint32_t;

inline constexpr EventBmp EventIn = 0x001;
inline constexpr EventBmp EventOut = 0x004;
inline constexpr EventBmp EventEt = 1U << 31;

}  // namespace aeronet