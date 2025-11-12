#include "aeronet/timestring.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/cctype.hpp"
#include "aeronet/config.hpp"
#include "aeronet/ipow.hpp"
#include "aeronet/log.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/year-week.hpp"

namespace aeronet {

SysTimePoint StringToTimeISO8601UTC(const char* begPtr, const char* endPtr) {
  const auto sz = endPtr - begPtr;
  if (AERONET_UNLIKELY(sz < 4)) {
    log::critical("ISO8601 Time string '{}' is too short, expected at least 4 characters",
                  std::string_view(begPtr, endPtr));
    throw std::invalid_argument("ISO8601 Time string too short");
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
    log::critical("Invalid date or time in ISO8601 Time string '{}'", std::string_view(begPtr, endPtr));
    throw std::invalid_argument("Invalid date or time in ISO8601 Time string");
  }

  SysTimePoint ts = std::chrono::sys_days{ymd} + std::chrono::hours{hours} + std::chrono::minutes{minutes} +
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

std::pair<SysTimePoint, SysTimePoint> ParseTimeWindow(std::string_view str) {
  if (str.size() < 4) {
    log::critical("Invalid time window string '{}' - expected at least a year YYYY", str);
    throw std::invalid_argument("Invalid time window string - too short");
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

    return {SysTimePoint(fromSysDays), SysTimePoint(toSysDays)};
  }
  if (*ptr == '-') {
    ++ptr;
  }

  // month or week number
  const auto dashPtr = std::find(ptr, endPtr, '-');
  if (dashPtr == ptr) {
    log::critical("Invalid time window string '{}' - expected a single dash after the year YYYY", str);
    throw std::invalid_argument("Invalid time window string - unexpected dash");
  }

  if (*ptr == 'W') {
    const iso_week_date isoWeekYear(static_cast<std::uint16_t>(yearNum), static_cast<unsigned int>(read2(ptr + 1)),
                                    std::chrono::Monday);
    const std::chrono::sys_days isoWeekFirstDay = isoWeekYear;

    return {SysTimePoint(isoWeekFirstDay), SysTimePoint(isoWeekFirstDay + std::chrono::days{7})};
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

    return {SysTimePoint(fromSysDays), SysTimePoint(toSysDays)};
  }
  // day
  const auto dayOfMonth = read2(ptr + 1);
  const std::chrono::year_month_day from{std::chrono::year{yearNum},
                                         std::chrono::month{static_cast<unsigned>(monthOfYear)},
                                         std::chrono::day{static_cast<unsigned>(dayOfMonth)}};
  const std::chrono::sys_days fromSysDays = from;
  const std::chrono::sys_days toSysDays = fromSysDays + std::chrono::days{1};

  return {SysTimePoint(fromSysDays), SysTimePoint(toSysDays)};
}

SysTimePoint TryParseTimeRFC7231(const char* begPtr, const char* endPtr) {
  SysTimePoint ret = kInvalidTimePoint;
  while (begPtr < endPtr && isspace(*begPtr)) {
    ++begPtr;
  }
  while (endPtr > begPtr && isspace(*(endPtr - 1))) {
    --endPtr;
  }

  if (begPtr >= endPtr) {
    return ret;
  }

  const auto len = endPtr - begPtr;
  if (std::cmp_not_equal(len, kRFC7231DateStrLen)) {
    return ret;  // Expect strict IMF-fixdate form
  }

  const char* ptr = begPtr;
  if (ptr[3] != ',' || ptr[4] != ' ' || ptr[7] != ' ' || ptr[11] != ' ' || ptr[16] != ' ' || ptr[19] != ':' ||
      ptr[22] != ':' || ptr[25] != ' ') {
    return ret;
  }

  if (!isdigit(ptr[5]) || !isdigit(ptr[6]) || !isdigit(ptr[12]) || !isdigit(ptr[13]) || !isdigit(ptr[14]) ||
      !isdigit(ptr[15]) || !isdigit(ptr[17]) || !isdigit(ptr[18]) || !isdigit(ptr[20]) || !isdigit(ptr[21]) ||
      !isdigit(ptr[23]) || !isdigit(ptr[24])) {
    return ret;
  }
  if (ptr[26] != 'G' || ptr[27] != 'M' || ptr[28] != 'T') {
    return ret;
  }

  static constexpr std::string_view kMonths[]{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  const std::string_view monthToken(ptr + 8, 3);

  const auto monthIt = std::ranges::find(kMonths, monthToken);
  if (monthIt == std::end(kMonths)) {
    return ret;
  }

  const int dayValue = read2(ptr + 5);
  const int yearValue = read4(ptr + 12);
  const int hourValue = read2(ptr + 17);
  const int minuteValue = read2(ptr + 20);
  const int secondValue = read2(ptr + 23);

  if (dayValue <= 0 || hourValue < 0 || hourValue > 23 || minuteValue < 0 || minuteValue > 59 || secondValue < 0 ||
      secondValue > 60) {
    return ret;
  }

  const std::chrono::year yearField{yearValue};
  // monthIt is a 0-based index into kMonths (0 == Jan). std::chrono::month is 1-based, so add 1.
  const std::chrono::month monthField{static_cast<unsigned>(monthIt - std::begin(kMonths)) + 1};
  const std::chrono::day dayField{static_cast<unsigned>(dayValue)};
  const std::chrono::year_month_day ymd{yearField, monthField, dayField};
  // Verify the weekday token (e.g. "Sun") matches the resolved date
  static constexpr std::string_view kWeekdays[]{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  const std::string_view weekdayToken(ptr, 3);
  const auto weekdayIt = std::ranges::find(kWeekdays, weekdayToken);
  if (weekdayIt == std::end(kWeekdays)) {
    return ret;
  }
  if (!ymd.ok()) {
    return ret;
  }

  const std::chrono::sys_days dayPoint{ymd};
  // ensure weekday token matches the computed weekday for the date
  const std::chrono::weekday wd{dayPoint};
  if (static_cast<unsigned>(weekdayIt - std::begin(kWeekdays)) != wd.c_encoding()) {
    return ret;
  }

  ret =
      dayPoint + std::chrono::hours{hourValue} + std::chrono::minutes{minuteValue} + std::chrono::seconds{secondValue};
  return ret;
}

}  // namespace aeronet
