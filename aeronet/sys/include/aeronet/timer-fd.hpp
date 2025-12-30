#pragma once

#include <chrono>
#include <cstdint>

#include "aeronet/base-fd.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

// Simple RAII wrapper around Linux timerfd.
// Used to trigger periodic maintenance work from an epoll-based event loop without relying on epoll_wait timeouts.
class TimerFd {
 public:
  // Create a disabled timerfd (non-blocking, close-on-exec).
  TimerFd();

  // Create and arm a periodic timer.
  explicit TimerFd(SysDuration interval);

  // Arm a periodic timer. A non-positive interval disables the timer.
  void armPeriodic(SysDuration interval) const;

  // Drain expirations (non-blocking). Safe to call even if the timer has not fired.
  void drain() const noexcept;

  [[nodiscard]] int fd() const noexcept { return _baseFd.fd(); }

 private:
  BaseFd _baseFd;
};

}  // namespace aeronet
