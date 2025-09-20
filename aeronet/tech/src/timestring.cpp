#include "timestring.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string_view>
#include <utility>

#include "config.hpp"
#include "invalid_argument_exception.hpp"
#include "ipow.hpp"
#include "simple-charconv.hpp"
#include "stringconv.hpp"
#include "timedef.hpp"
#include "year-week.hpp"

namespace aeronet {

TimePoint StringToTimeISO8601UTC(const char* begPtr, const char* endPtr) {
  const auto sz = endPtr - begPtr;
  if (AERONET_UNLIKELY(sz < 4)) {
    throw invalid_argument("ISO8601 Time string '{}' is too short, expected at least 4 characters",
                           std::string_view(begPtr, endPtr));
  }

  std::chrono::year year(read4(begPtr));
  std::chrono::month month = std::chrono::January;
  std::chrono::day day = std::chrono::day{1};
  uint8_t hours = 0;
  uint8_t minutes = 0;
  uint8_t seconds = 0;

  const auto* begSuffix = begPtr + 10;

  if (AERONET_LIKELY(sz >= 7)) {
    month = std::chrono::month(static_cast<unsigned>(read2(begPtr + 5)));
    if (AERONET_LIKELY(sz >= 10)) {
      day = std::chrono::day(static_cast<unsigned>(read2(begPtr + 8)));
      if (sz >= 13) {
        hours = static_cast<uint8_t>(read2(begPtr + 11));
        begSuffix = begPtr + 13;
        if (sz >= 16) {
          minutes = static_cast<uint8_t>(read2(begPtr + 14));
          begSuffix = begPtr + 16;
          if (sz >= 19) {
            seconds = static_cast<uint8_t>(read2(begPtr + 17));
            begSuffix = begPtr + 19;
          }
        }
      }
    }
  }

  std::chrono::year_month_day ymd{year, month, day};

  // NOLINTNEXTLINE(readability-simplify-boolean-expr)
  if (AERONET_UNLIKELY(!ymd.ok() || hours > 23 || minutes > 59 || seconds > 60)) {  // 60 is possible with leap second
    throw invalid_argument("Invalid date or time in ISO8601 Time string '{}'", std::string_view(begPtr, endPtr));
  }

  TimePoint ts = std::chrono::sys_days{ymd} + std::chrono::hours{hours} + std::chrono::minutes{minutes} +
                 std::chrono::seconds{seconds};

  // For inputs like this,
  // 'begSuffix' will point here:
  //              |
  //              v
  //  - YYYY-MM-DDT
  //                 |
  //                 v
  //  - YYYY-MM-DDTHHZ
  //                    |
  //                    v
  //  - YYYY-MM-DDTHH:MM+05:00
  //                       |
  //                       v
  //  - YYYY-MM-DDTHH:MM:SS.sssZ
  if (begSuffix < endPtr) {
    // Parse local time offset, if present
    if (*(endPtr - 1) == 'Z') {
      // remove the Z, consider UTC anyways
      --endPtr;
    } else if (*(endPtr - 6) == '-') {
      ts += std::chrono::hours{read2(endPtr - 5)} + std::chrono::minutes{read2(endPtr - 2)};
      endPtr -= 6;
    } else if (*(endPtr - 6) == '+') {
      ts -= std::chrono::hours{read2(endPtr - 5)} + std::chrono::minutes{read2(endPtr - 2)};
      endPtr -= 6;
    }

    if (*begSuffix == '.') {
      // parse sub-seconds parts until the end (possible 'Z' removed)
      const auto subSecondsSz = std::min<long>(endPtr - ++begSuffix, 9);
      switch (subSecondsSz) {
        case 0:
          break;
        case 3:  // milliseconds
          ts += std::chrono::milliseconds{read3(begSuffix)};
          break;
        case 6:  // microseconds
          ts += std::chrono::microseconds{read6(begSuffix)};
          break;
        case 9:  // nanoseconds
          ts += std::chrono::nanoseconds{read9(begSuffix)};
          break;
        default:
          ts += std::chrono::nanoseconds{StringToIntegral<int32_t>(begSuffix, static_cast<std::size_t>(subSecondsSz)) *
                                         static_cast<int32_t>(ipow10(static_cast<uint8_t>(9 - subSecondsSz)))};
          break;
      }
    }
  }

  return ts;
}

std::pair<TimePoint, TimePoint> ParseTimeWindow(std::string_view str) {
  if (str.size() < 4) {
    throw invalid_argument("Invalid time window string '{}' - expected at least a year YYYY", str);
  }

  const char* ptr = str.data();
  const char* endPtr = str.data() + str.size();

  // year
  const int yearNum = read4(ptr);
  ptr += 4;
  if (ptr == endPtr) {
    const std::chrono::year_month_day from{std::chrono::year{yearNum}, std::chrono::January, std::chrono::day{1}};
    const std::chrono::year_month_day to = from + std::chrono::years{1};
    const std::chrono::sys_days fromSysDays = from;
    const std::chrono::sys_days toSysDays = to;

    return {TimePoint(fromSysDays), TimePoint(toSysDays)};
  }
  if (*ptr == '-') {
    ++ptr;
  }

  // month or week number
  const auto dashPtr = std::find(ptr, endPtr, '-');
  if (dashPtr == ptr) {
    throw invalid_argument("Invalid time window string '{}' - expected a single dash after the year YYYY", str);
  }

  if (*ptr == 'W') {
    const iso_week_date isoWeekYear(static_cast<std::uint16_t>(yearNum), static_cast<unsigned int>(read2(ptr + 1)),
                                    std::chrono::Monday);
    const std::chrono::sys_days isoWeekFirstDay = isoWeekYear;

    return {TimePoint(isoWeekFirstDay), TimePoint(isoWeekFirstDay + std::chrono::days{7})};
  }

  // month
  const auto monthOfYear = read2(ptr);
  ptr = dashPtr;

  if (ptr == endPtr) {
    const std::chrono::year_month_day from{std::chrono::year{yearNum},
                                           std::chrono::month{static_cast<unsigned>(monthOfYear)}, std::chrono::day{1}};
    const std::chrono::year_month_day to = from + std::chrono::months{1};
    const std::chrono::sys_days fromSysDays = from;
    const std::chrono::sys_days toSysDays = to;

    return {TimePoint(fromSysDays), TimePoint(toSysDays)};
  }
  // day
  const auto dayOfMonth = read2(ptr + 1);
  const std::chrono::year_month_day from{std::chrono::year{yearNum},
                                         std::chrono::month{static_cast<unsigned>(monthOfYear)},
                                         std::chrono::day{static_cast<unsigned>(dayOfMonth)}};
  const std::chrono::sys_days fromSysDays = from;
  const std::chrono::sys_days toSysDays = fromSysDays + std::chrono::days{1};

  return {TimePoint(fromSysDays), TimePoint(toSysDays)};
}

}  // namespace aeronet
