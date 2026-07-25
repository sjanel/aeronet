#include "aeronet/timestring.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string_view>
#include <utility>

#include "aeronet/cctype.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

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
  if (std::cmp_not_equal(len, RFC7231DateStrLen)) {
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

  static constexpr uint16_t kGMTRep = read3("GMT");

  if (read3(ptr + 26) != kGMTRep) {
    return ret;
  }

  static constexpr uint16_t kMonthsRep[]{
      read3("Jan"), read3("Feb"), read3("Mar"), read3("Apr"), read3("May"), read3("Jun"),
      read3("Jul"), read3("Aug"), read3("Sep"), read3("Oct"), read3("Nov"), read3("Dec"),
  };

  const auto monthIt = std::ranges::find(kMonthsRep, read3(ptr + 8));
  if (monthIt == std::end(kMonthsRep)) {
    return ret;
  }

  const int dayValue = read2(ptr + 5);
  const int yearValue = read4(ptr + 12);
  const int hourValue = read2(ptr + 17);
  const int minuteValue = read2(ptr + 20);
  const int secondValue = read2(ptr + 23);

  if (dayValue == 0 || hourValue > 23 || minuteValue > 59 || secondValue > 60) {
    return ret;
  }

  const std::chrono::year yearField{yearValue};
  // monthIt is a 0-based index into kMonths (0 == Jan). std::chrono::month is 1-based, so add 1.
  const std::chrono::month monthField{static_cast<unsigned>(monthIt - std::begin(kMonthsRep)) + 1};
  const std::chrono::day dayField{static_cast<unsigned>(dayValue)};
  const std::chrono::year_month_day ymd{yearField, monthField, dayField};
  // Verify the weekday token (e.g. "Sun") matches the resolved date
  static constexpr uint16_t kWeekdays[]{
      read3("Sun"), read3("Mon"), read3("Tue"), read3("Wed"), read3("Thu"), read3("Fri"), read3("Sat"),
  };
  const auto weekdayIt = std::ranges::find(kWeekdays, read3(ptr));
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
