#pragma once

#include <cstddef>
#include <cstdint>

#include "aeronet/chrono-lite.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

inline constexpr std::size_t ISO8601UTCWithMsStrLen = 24;  // "YYYY-MM-DDThh:mm:ss.sssZ"
inline constexpr std::size_t RFC7231DateStrLen = 29;       // "Sun, 23 Aug 2026 18:51:26 GMT"
inline constexpr SysTimePoint kInvalidTimePoint = SysTimePoint::max();

/// The time point 'secondsSinceEpoch' seconds plus 'nanoseconds' (0 to 999,999,999) after the Unix epoch, clamped to
/// the range of SysTimePoint (years 1678 to 2262 with the nanosecond ticks of libstdc++, far more with the microseconds
/// of libc++ and the 100 ns of MSVC): a file time or a date beyond it does not overflow. Never kInvalidTimePoint.
[[nodiscard]] constexpr SysTimePoint SaturatedSysTimePoint(std::int64_t secondsSinceEpoch,
                                                           std::int64_t nanoseconds = 0) noexcept {
  using std::chrono::seconds;
  // The extreme whole seconds whose sum with a sub-second part still fits SysDuration.
  constexpr std::int64_t kMinSeconds = std::chrono::ceil<seconds>(SysDuration::min()).count();
  constexpr std::int64_t kMaxSeconds = std::chrono::floor<seconds>(SysDuration::max()).count() - 1;
  if (secondsSinceEpoch < kMinSeconds) {
    secondsSinceEpoch = kMinSeconds;
  } else if (secondsSinceEpoch > kMaxSeconds) {
    secondsSinceEpoch = kMaxSeconds;
  }
  return SysTimePoint{std::chrono::duration_cast<SysDuration>(seconds{secondsSinceEpoch}) +
                      std::chrono::duration_cast<SysDuration>(std::chrono::nanoseconds{nanoseconds})};
}

}  // namespace aeronet
