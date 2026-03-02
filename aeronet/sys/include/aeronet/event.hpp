#pragma once

#include <cstdint>

namespace aeronet {

using EventBmp = uint32_t;

// Platform-abstract event flags.
// On Linux, implementations verify these match the corresponding EPOLL* values.
// On macOS, the kqueue backend maps to/from native kevent flags internally.
// On Windows, the IOCP backend maps to/from completion-based semantics internally.
inline constexpr EventBmp EventIn = 0x001;
inline constexpr EventBmp EventOut = 0x004;
inline constexpr EventBmp EventErr = 0x008;
inline constexpr EventBmp EventHup = 0x010;
inline constexpr EventBmp EventRdHup = 0x2000;
inline constexpr EventBmp EventEt = 1U << 31;

}  // namespace aeronet