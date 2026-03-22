#pragma once

#include <cstdint>

namespace aeronet {

using EventBmp = uint32_t;

// Platform-abstract event flags.
// On Linux, implementations verify these match the corresponding EPOLL* values.
// On macOS, the kqueue backend maps to/from native kevent flags internally.
// On Windows, the WSAPoll backend maps to/from poll-based semantics internally.
inline constexpr EventBmp EventIn = 0x001;
inline constexpr EventBmp EventOut = 0x004;
inline constexpr EventBmp EventErr = 0x008;
inline constexpr EventBmp EventHup = 0x010;
inline constexpr EventBmp EventRdHup = 0x2000;
inline constexpr EventBmp EventEt = 1U << 31;

// Synthetic flag set on CQEs produced by completion-based accept (io_uring IORING_OP_ACCEPT).
// When set, EventFd.fd is the NEW accepted client fd, not the listen socket.
// On non-io_uring backends this flag is never set; accept is readiness-based.
inline constexpr EventBmp EventAccept = 1U << 30;

// Mask to strip edge-triggered and synthetic flags when converting to poll(2) masks
// (used by io_uring IORING_OP_POLL_ADD which uses poll semantics).
inline constexpr EventBmp EventPollMask = ~(EventEt | EventAccept);

}  // namespace aeronet