#pragma once

// The part of <chrono> that the aeronet headers and most of its sources use: durations (with floor, ceil, round, abs
// and the h, min, s, ms, us and ns literals), time points (with sys_time, sys_seconds and sys_days), system_clock and
// steady_clock. Since C++20, <chrono> also holds the calendar, the time zones and the formatting of durations and
// dates, and includes <format>, <string>, <sstream>...: about 300 ms of compile time per translation unit, against
// about 10 ms for this subset (clang 23, libstdc++ 13). Include <chrono> for the rest (year_month_day, weekday,
// hh_mm_ss, clock_cast, formatting or streaming a duration).
//
// libstdc++ and libc++ declare this subset in internal headers, used when present. Otherwise, and with the MSVC STL
// whose <chrono> defines system_clock itself, it is the whole <chrono>: only the compile time differs.
//
// include-cleaner attributes the standard chrono symbols to <chrono> only: .clang-tidy does not require <chrono>, and
// this pragma keeps the includes of this header.
// IWYU pragma: always_keep

#include <version>  // IWYU pragma: keep (defines __GLIBCXX__ or _LIBCPP_VERSION)

#if defined(__GLIBCXX__) && __has_include(<bits/chrono.h>)
#include <bits/chrono.h>  // IWYU pragma: export
#elif defined(_LIBCPP_VERSION) && __has_include(<__chrono/system_clock.h>) && __has_include(<__chrono/steady_clock.h>)
#include <__chrono/duration.h>      // IWYU pragma: export
#include <__chrono/steady_clock.h>  // IWYU pragma: export
#include <__chrono/system_clock.h>  // IWYU pragma: export
#include <__chrono/time_point.h>    // IWYU pragma: export
#else
#include <chrono>  // IWYU pragma: export
#endif
