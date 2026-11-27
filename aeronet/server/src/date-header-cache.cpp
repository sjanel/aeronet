#include "aeronet/internal/date-header-cache.hpp"

#include <ctime>

#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet::internal {

void DateHeaderCache::refresh(std::time_t second) noexcept {
  _second = second;
  TimeToStringRFC7231(SysClock::from_time_t(second), _header);
}

}  // namespace aeronet::internal
