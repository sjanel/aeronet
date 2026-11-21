#pragma once

#include <chrono>

namespace aeronet {

/// Alias some types to make it easier to use
/// The main clock is system_clock as it is the only one guaranteed to provide conversions to Unix epoch time.
/// It is not monotonic - thus for unit tests we will prefer usage of steady_clock
using SysClock = std::chrono::system_clock;
using SysTimePoint = SysClock::time_point;
using SysDuration = SysClock::duration;

using SteadyClock = std::chrono::steady_clock;

/// 'start' + 'timeout', saturated at SteadyClock::time_point::max(): a timeout beyond the range of the clock
/// (std::chrono::milliseconds::max() for instance) never expires, without overflowing the nanoseconds of the clock.
[[nodiscard]] constexpr SteadyClock::time_point DeadlineAfter(SteadyClock::time_point start,
                                                              std::chrono::milliseconds timeout) noexcept {
  return timeout < std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::time_point::max() - start)
             ? start + timeout
             : SteadyClock::time_point::max();
}

}  // namespace aeronet
