#pragma once

#include "aeronet/base-fd.hpp"
#include "aeronet/platform.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

// RAII wrapper around a periodic timer.
// Linux  : timerfd (non-blocking, close-on-exec)
// macOS  : self-pipe with alarm (the event loop can also use EVFILT_TIMER natively,
//          but using a pipe-based fd keeps the EventLoop interface uniform)
// Windows: waitable timer stub
class TimerFd {
 public:
  // Create a disabled timer.
  TimerFd();

  // Arm a periodic timer. A non-positive interval disables the timer.
  void armPeriodic(SysDuration interval) const;

  // Drain expirations (non-blocking). Safe to call even if the timer has not fired.
  void drain() const noexcept;

  [[nodiscard]] NativeHandle fd() const noexcept { return _baseFd.fd(); }

 private:
  BaseFd _baseFd;
#ifdef AERONET_MACOS
  BaseFd _writeFd;  // write end of timer pipe (used by kqueue EVFILT_TIMER callback path)
#endif
};

}  // namespace aeronet
