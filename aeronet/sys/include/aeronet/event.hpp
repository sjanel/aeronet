#pragma once

#include <cstdint>

namespace aeronet {

using EventBmp = uint32_t;

// Platform-abstract event flags.
// On Linux, implementations verify these match the corresponding EPOLL* values.
// On macOS, the kqueue backend maps to/from native kevent flags internally.
// On Windows, the WSAPoll backend maps to/from poll-based semantics internally.
inline constexpr EventBmp EventIn = 0x001U;
inline constexpr EventBmp EventOut = 0x004U;
inline constexpr EventBmp EventErr = 0x008U;
inline constexpr EventBmp EventHup = 0x010U;
inline constexpr EventBmp EventRdHup = 0x2000U;
inline constexpr EventBmp EventEt = 1U << 31U;

}  // namespace aeronet