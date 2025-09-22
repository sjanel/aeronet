#pragma once

#include <chrono>

namespace aeronet {

/// Alias some types to make it easier to use
/// The main clock is system_clock as it is the only one guaranteed to provide conversions to Unix epoch time.
/// It is not monotonic - thus for unit tests we will prefer usage of steady_clock
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

}  // namespace aeronet
