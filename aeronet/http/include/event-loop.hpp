#pragma once

#include <sys/epoll.h>

#include <cstddef>
#include <cstdint>
#include <functional>

#include "base-fd.hpp"
#include "timedef.hpp"
#include "vector.hpp"

namespace aeronet {

// Thin RAII wrapper over an epoll instance.
//
// Design notes:
//  * Inherits from BaseFd to reuse unified close()/logging + move semantics.
//  * Event buffer starts with kInitialCapacity (64). Rationale:
//      - Large enough to avoid immediate reallocations for small / moderate servers.
//      - 64 epoll_event structs are tiny (typically 12–16 bytes each) => < 1 KB.
//      - Keeps stack/heap churn low in the common path while not over‑allocating.
//    On saturation (returned events == current capacity) the vector is doubled.
//    This exponential growth yields amortized O(1) reallocation behaviour and
//    quickly reaches an adequate size for higher concurrency (64 -> 128 -> 256 ...).
//  * We do not shrink the buffer; epoll_wait cost is independent of capacity and
//    keeping the memory avoids oscillations under fluctuating load.
//  * add()/mod()/del() return success/failure and log details on failure; caller
//    can decide policy (e.g., drop connection / abort).
class EventLoop : public BaseFd {
 public:
  static constexpr std::size_t kInitialCapacity = 64;

  // Construct an EventLoop.
  // Parameters:
  //   epollFlags       -> flags passed to epoll_create1 (e.g. EPOLL_CLOEXEC). 0 for none.
  //   initialCapacity  -> starting number of epoll_event slots reserved in the internal buffer.
  //                       Must be > 0. Values <= 0 are promoted to 1. A value of 64 is a good
  //                       balance for small/medium workloads: it fits easily in cache (< 1 KB)
  //                       yet avoids immediate reallocations. Buffer grows by doubling whenever
  //                       a poll returns exactly capacity() events. It never shrinks.
  explicit EventLoop(int epollFlags = 0, std::size_t initialCapacity = kInitialCapacity);

  [[nodiscard]] bool add(int fd, uint32_t events) const;

  [[nodiscard]] bool mod(int fd, uint32_t events) const;

  void del(int fd) const;

  // Polls for ready events up to (timeout). On success returns number of ready fds.
  // Returns 0 when interrupted by a signal (EINTR handled internally) or when timeout expires with no events.
  // Returns -1 on unrecoverable epoll_wait failure (already logged).
  int poll(Duration timeout, const std::function<void(int fd, uint32_t ev)>& cb);

  // Current allocated capacity (number of epoll_event slots available without reallocation).
  [[nodiscard]] std::size_t capacity() const noexcept { return _events.size(); }

  // Number of ready file descriptors returned by the most recent successful poll().
  // Reset to 0 on EINTR or on unrecoverable error (-1 return).
  [[nodiscard]] int size() const noexcept { return _lastReadyCount; }

 private:
  vector<epoll_event> _events;
  int _lastReadyCount = 0;  // last nbReadyFds observed; 0 if none or last poll interrupted/failed
};

}  // namespace aeronet
