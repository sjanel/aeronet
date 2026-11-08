#include "aeronet/timestring.hpp"

#include <chrono>
#include <cstddef>
#include <string_view>

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

  const std::size_t len = static_cast<std::size_t>(endPtr - begPtr);
  if (len != RFC7231DateStrLen) {
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

  // The letters are compared exactly (read3() is for digits only: "Npl" would read as "Nov").
  if (std::string_view(ptr + 26, 3U) != "GMT") {
    return ret;
  }

  const unsigned month = MonthOfAbbreviation(ptr + 8);
  if (month == 0) {
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

  const std::chrono::year_month_day ymd{std::chrono::year{yearValue}, std::chrono::month{month},
                                        std::chrono::day{static_cast<unsigned>(dayValue)}};
  if (!ymd.ok()) {
    return ret;
  }

  const std::chrono::sys_days dayPoint{ymd};
  // The weekday token (e.g. "Sun") must be the weekday of the date: an unknown one has the index 7, never equal.
  if (WeekdayOfAbbreviation(ptr) != std::chrono::weekday{dayPoint}.c_encoding()) {
    return ret;
  }

  ret =
      dayPoint + std::chrono::hours{hourValue} + std::chrono::minutes{minuteValue} + std::chrono::seconds{secondValue};
  return ret;
}

}  // namespace aeronet
