#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#ifdef AERONET_LINUX
#include <sys/epoll.h>
#endif

#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/base-fd.hpp"
#include "aeronet/event.hpp"
#include "aeronet/native-handle.hpp"

namespace aeronet {

#ifdef AERONET_LINUX
namespace detail {
// Padding helper to mirror epoll_event layout inside EventFd exactly.
// Sizes are derived from epoll_event via offsetof/sizeof — no arch-specific guards needed.
// EpollPad<0> is an empty type; [[no_unique_address]] ensures it occupies zero bytes.
template <std::size_t N>
struct alignas(1) EpollPad {
  uint8_t _[N];
};
template <>
struct EpollPad<0> {};
}  // namespace detail
#endif

// Thin RAII wrapper over a platform event-notification mechanism
// (epoll on Linux, kqueue on macOS, WSAPoll on Windows).
//
// Design notes:
//  * Inherits from BaseFd to reuse unified close()/logging + move semantics.
//  * Event buffer starts with kInitialCapacity (64). Rationale:
//      - Large enough to avoid immediate reallocations for small / moderate servers.
//      - Keeps stack/heap churn low in the common path while not over‑allocating.
//    On saturation (returned events == current capacity) the capacity is doubled.
//    This exponential growth yields amortized O(1) reallocation behaviour and
//    quickly reaches an adequate size for higher concurrency (64 -> 128 -> 256 ...).
//  * We do not shrink the buffer; poll cost is independent of capacity and
//    keeping the memory avoids oscillations under fluctuating load.
//  * add()/mod()/del() return success/failure and log details on failure; caller
//    can decide policy (e.g., drop connection / abort).
class EventLoop {
 public:
  static constexpr uint32_t kInitialCapacity = 64;

  struct EventFd {
    constexpr EventFd(NativeHandle fd, EventBmp eventBmp) : eventBmp(eventBmp), fd(fd) {}

    EventBmp eventBmp;
#ifdef AERONET_LINUX
    // Layout mirrors epoll_event exactly; sizes derived from epoll_event via offsetof/sizeof.
    // The static_assert in event-loop.cpp validates the result at compile time.
    [[no_unique_address]] detail::EpollPad<offsetof(epoll_event, data.fd) - sizeof(EventBmp)> _pre_fd_pad;
    NativeHandle fd;
    [[no_unique_address]] detail::EpollPad<sizeof(epoll_event) - offsetof(epoll_event, data.fd) - sizeof(NativeHandle)>
        _post_fd_pad;
#else
    NativeHandle fd;
#endif
  };

  // Default constructor - creates an empty EventLoop.
  EventLoop() noexcept = default;

  // Construct an EventLoop with a fixed or adaptive poll timeout policy.
  // Parameters:
  //   timeoutPolicy    -> validated poll timeout policy; validate() is called here.
  //   initialCapacity  -> starting number of event slots reserved in the internal buffer.
  //                       Must be > 0. Values <= 0 are promoted to 1. A value of 64 is a good
  //                       balance for small/medium workloads: it fits easily in cache (< 1 KB)
  //                       yet avoids immediate reallocations. Buffer grows by doubling whenever
  //                       a poll returns exactly capacity() events. It never shrinks.
  explicit EventLoop(PollTimeoutPolicy timeoutPolicy, uint32_t initialCapacity = kInitialCapacity);

  EventLoop(const EventLoop&) = delete;
  EventLoop(EventLoop&& rhs) noexcept;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop& operator=(EventLoop&& rhs) noexcept;

  ~EventLoop();

  // Register fd with given events.
  // On error, throws std::system_error.
  void addOrThrow(EventFd event);

  // Register fd with given events.
  // Returns true on success, false on failure (logged).
  [[nodiscard]] bool add(EventFd event);

  // Modify fd with given events.
  // Returns true on success, false on failure (logged).
  [[nodiscard]] bool mod(EventFd event);

  // Delete fd from monitoring.
  // Log on error.
  void del(NativeHandle fd);

  // Polls for ready events up to the poll timeout.
  //
  // Returns a span over an internal, reusable buffer (no allocations on the hot path).
  //
  // Semantics:
  //  - On success: returns a non-empty span of ready events.
  //  - On timeout or when interrupted by a signal (error::kInterrupted): returns an empty span
  //    with non-null data() pointer.
  //  - On unrecoverable poll failure (already logged): returns an empty span
  //    with nullptr data() pointer.
  [[nodiscard]] std::span<const EventFd> poll();

  // Current allocated capacity (number of event slots available without reallocation).
  [[nodiscard]] uint32_t capacity() const noexcept { return _nbAllocatedEvents; }

  // Current effective poll timeout in milliseconds.
  [[nodiscard]] int currentPollTimeoutMs() const noexcept { return _timeout.pollTimeoutMs(); }

  // Update the fixed or adaptive poll timeout policy.
  void updatePollTimeoutPolicy(PollTimeoutPolicy timeoutPolicy) { _timeout = AdaptivePollTimeout(timeoutPolicy); }

 private:
  uint32_t _nbAllocatedEvents = 0;
  AdaptivePollTimeout _timeout;
  BaseFd _baseFd;
  void* _pEvents = nullptr;
#ifdef AERONET_WINDOWS
  void* _pPollFds = nullptr;   // WSAPOLLFD registration array for WSAPoll()
  uint32_t _nbRegistered = 0;  // number of fds currently registered
#endif
};

}  // namespace aeronet
