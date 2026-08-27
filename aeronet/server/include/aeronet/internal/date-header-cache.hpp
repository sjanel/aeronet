#pragma once

#include <chrono>
#include <string_view>

#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet::internal {

// Single-reactor cache for the fixed-width HTTP Date header. The cache is refreshed lazily at wall-clock second
// boundaries, using the event loop's existing monotonic timestamp to avoid a system-clock read on every response.
class DateHeaderCache {
 public:
  void refreshIfNeeded(SteadyClock::time_point steadyNow) {
    refreshIfNeeded(steadyNow, [] { return SysClock::now(); });
  }

  template <typename WallNowFn>
  void refreshIfNeeded(SteadyClock::time_point steadyNow, WallNowFn&& wallNowFn) {
    if (steadyNow < _refreshAt) {
      return;
    }

    const SysTimePoint wallNow = wallNowFn();
    const auto wallSecond = std::chrono::floor<std::chrono::seconds>(wallNow);
    TimeToStringRFC7231(wallSecond, _header);

    // Align the next refresh to the wall-clock boundary. Refreshing at steadyNow + 1s would let a cache initialized
    // near the end of a wall second remain one full second behind for almost the entire following second.
    const auto untilNextSecond = std::chrono::seconds{1} - (wallNow - wallSecond);
    _refreshAt = steadyNow + std::chrono::ceil<SteadyClock::duration>(untilNextSecond);
  }

  // Returns a pointer to the date buffer.
  // Beware - it's not null terminated, and the length is RFC7231DateStrLen.
  [[nodiscard]] const char* data() const noexcept { return _header; }

  [[nodiscard]] std::string_view view() const noexcept { return {_header, sizeof(_header)}; }

 private:
  SteadyClock::time_point _refreshAt{SteadyClock::time_point::min()};
  char _header[RFC7231DateStrLen]{};
};

}  // namespace aeronet::internal
