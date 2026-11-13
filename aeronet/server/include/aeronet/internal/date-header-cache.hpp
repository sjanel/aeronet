#pragma once

#include <ctime>
#include <limits>

#include "aeronet/compiler-config.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet::internal {

// Single-reactor cache for the fixed-width HTTP Date header. Each response reads the wall clock in whole seconds
// (std::time: a load from the vDSO data page on Linux, about 1 ns) and the date is formatted only when the second
// changed. The Date of a response is thus the second of its generation (RFC 9110 §6.6.1), also after a slow handler,
// and the date of the start of an event loop iteration is never reused for the responses built later in it.
class DateHeaderCache {
 public:
  // The wall clock in whole seconds since the epoch, with the signature of std::time. Tests inject a fixed clock.
  using WallSecondsFn = std::time_t (*)(std::time_t*);

  DateHeaderCache() noexcept = default;

  explicit DateHeaderCache(WallSecondsFn wallSeconds) noexcept : _wallSeconds(wallSeconds) {}

  // Returns the Date header value of the current second.
  // It's not null terminated, and the length is RFC7231DateStrLen.
  [[nodiscard]] const char* current() noexcept {
    const std::time_t second = _wallSeconds(nullptr);
    if (second != _second) [[unlikely]] {
      refresh(second);
    }
    return _header;
  }

 private:
  AERONET_NOINLINE void refresh(std::time_t second) noexcept {
    _second = second;
    TimeToStringRFC7231(SysClock::from_time_t(second), _header);
  }

  WallSecondsFn _wallSeconds{&std::time};
  std::time_t _second{std::numeric_limits<std::time_t>::min()};
  char _header[RFC7231DateStrLen]{};
};

}  // namespace aeronet::internal
