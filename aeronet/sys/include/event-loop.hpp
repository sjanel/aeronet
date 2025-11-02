#pragma once

#include <cstdint>
#include <functional>

#include "base-fd.hpp"
#include "event.hpp"
#include "timedef.hpp"

namespace aeronet {

// Thin RAII wrapper over an epoll instance.
//
// Design notes:
//  * Inherits from BaseFd to reuse unified close()/logging + move semantics.
//  * Event buffer starts with kInitialCapacity (64). Rationale:
//      - Large enough to avoid immediate reallocations for small / moderate servers.
//      - 64 epoll_event structs are tiny (typically 12–16 bytes each) => < 1 KB.
//      - Keeps stack/heap churn low in the common path while not over‑allocating.
//    On saturation (returned events == current capacity) the capacity is doubled.
//    This exponential growth yields amortized O(1) reallocation behaviour and
//    quickly reaches an adequate size for higher concurrency (64 -> 128 -> 256 ...).
//  * We do not shrink the buffer; epoll_wait cost is independent of capacity and
//    keeping the memory avoids oscillations under fluctuating load.
//  * add()/mod()/del() return success/failure and log details on failure; caller
//    can decide policy (e.g., drop connection / abort).
class EventLoop {
 public:
  static constexpr uint32_t kInitialCapacity = 64;

  struct EventFd {
    int fd;
    EventBmp eventBmp;
  };

  // Default constructor - creates an empty EventLoop.
  EventLoop() noexcept = default;

  // Construct an EventLoop.
  // Parameters:
  //   pollTimeout      -> timeout for poll() calls
  //   epollFlags       -> flags passed to epoll_create1 (e.g. EPOLL_CLOEXEC). 0 for none.
  //   initialCapacity  -> starting number of epoll_event slots reserved in the internal buffer.
  //                       Must be > 0. Values <= 0 are promoted to 1. A value of 64 is a good
  //                       balance for small/medium workloads: it fits easily in cache (< 1 KB)
  //                       yet avoids immediate reallocations. Buffer grows by doubling whenever
  //                       a poll returns exactly capacity() events. It never shrinks.
  explicit EventLoop(SysDuration pollTimeout, int epollFlags = 0, uint32_t initialCapacity = kInitialCapacity);

  EventLoop(const EventLoop&) = delete;
  EventLoop(EventLoop&& rhs) noexcept;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop& operator=(EventLoop&& rhs) noexcept;

  ~EventLoop();

  // Register fd with given events.
  // On error, throws std::system_error.
  void add_or_throw(EventFd event) const;

  // Register fd with given events.
  // Returns true on success, false on failure (logged).
  [[nodiscard]] bool add(EventFd event) const;

  // Modify fd with given events.
  // Returns true on success, false on failure (logged).
  [[nodiscard]] bool mod(EventFd event) const;

  // Delete fd from epoll monitoring.
  void del(int fd) const;

  // Polls for ready events up to the poll timeout. On success returns number of ready fds.
  // Returns 0 when interrupted by a signal (EINTR handled internally) or when timeout expires with no events.
  // Returns -1 on unrecoverable epoll_wait failure (already logged).
  int poll(const std::function<void(EventFd event)>& cb);

  // Current allocated capacity (number of epoll_event slots available without reallocation).
  [[nodiscard]] uint32_t capacity() const noexcept { return _nbAllocatedEvents; }

 private:
  uint32_t _nbAllocatedEvents = 0;
  int _pollTimeoutMs = 0;
  BaseFd _baseFd;
  void* _pEvents = nullptr;
};

}  // namespace aeronet
