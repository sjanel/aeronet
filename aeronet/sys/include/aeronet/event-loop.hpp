#pragma once

#include <cstdint>
#include <span>

#include "aeronet/base-fd.hpp"
#include "aeronet/event.hpp"
#include "aeronet/platform.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

// Thin RAII wrapper over a platform event-notification mechanism
// (epoll on Linux, kqueue on macOS, IOCP stub on Windows).
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
    EventFd(NativeHandle fd, EventBmp eventBmp) : eventBmp(eventBmp), fd(fd) {}

    EventBmp eventBmp;
    NativeHandle fd;
#ifdef AERONET_POSIX
    uint32_t _padding;
#endif
  };

  // Default constructor - creates an empty EventLoop.
  EventLoop() noexcept = default;

  // Construct an EventLoop.
  // Parameters:
  //   pollTimeout      -> timeout for poll() calls
  //   initialCapacity  -> starting number of event slots reserved in the internal buffer.
  //                       Must be > 0. Values <= 0 are promoted to 1. A value of 64 is a good
  //                       balance for small/medium workloads: it fits easily in cache (< 1 KB)
  //                       yet avoids immediate reallocations. Buffer grows by doubling whenever
  //                       a poll returns exactly capacity() events. It never shrinks.
  explicit EventLoop(SysDuration pollTimeout, uint32_t initialCapacity = kInitialCapacity);

  EventLoop(const EventLoop&) = delete;
  EventLoop(EventLoop&& rhs) noexcept;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop& operator=(EventLoop&& rhs) noexcept;

  ~EventLoop();

  // Register fd with given events.
  // On error, throws std::system_error.
  void addOrThrow(EventFd event) const;

  // Register fd with given events.
  // Returns true on success, false on failure (logged).
  [[nodiscard]] bool add(EventFd event) const;

  // Modify fd with given events.
  // Returns true on success, false on failure (logged).
  [[nodiscard]] bool mod(EventFd event) const;

  // Delete fd from monitoring.
  // Log on error.
  void del(NativeHandle fd) const;

  // Polls for ready events up to the poll timeout.
  //
  // Returns a span over an internal, reusable buffer (no allocations on the hot path).
  //
  // Semantics:
  //  - On success: returns a non-empty span of ready events.
  //  - On timeout or when interrupted by a signal (EINTR): returns an empty span
  //    with non-null data() pointer.
  //  - On unrecoverable poll failure (already logged): returns an empty span
  //    with nullptr data() pointer.
  [[nodiscard]] std::span<const EventFd> poll();

  // Current allocated capacity (number of event slots available without reallocation).
  [[nodiscard]] uint32_t capacity() const noexcept { return _nbAllocatedEvents; }

  // Update the poll timeout.
  void updatePollTimeout(SysDuration pollTimeout);

 private:
  uint32_t _nbAllocatedEvents = 0;
  int _pollTimeoutMs = 0;
  BaseFd _baseFd;
  void* _pEvents = nullptr;
};

}  // namespace aeronet
