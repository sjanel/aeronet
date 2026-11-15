#include "aeronet/timestring.hpp"

#include <chrono>
#include <cstddef>
#include <string_view>

#include "aeronet/cctype.hpp"
#include "aeronet/compiler-config.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

namespace {

// Length of the part of an rfc850-date after its day name: ", 06-Nov-94 08:49:37 GMT".
constexpr std::size_t kRfc850DateTailLen = 24;
// Length of an asctime-date: "Sun Nov  6 08:49:37 1994".
constexpr std::size_t kAsctimeDateLen = 24;

// Full day names of rfc850-date, from Sunday (the order of std::chrono::weekday::c_encoding()).
constexpr std::string_view kWeekdayNames[]{
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

constexpr bool AreDigits(const char* ptr, std::size_t nbDigits) {
  for (std::size_t pos = 0; pos < nbDigits; ++pos) {
    if (!isdigit(ptr[pos])) {
      return false;
    }
  }
  return true;
}

// The obsolete formats reach the abbreviation tables through these: IMF-fixdate remains their only inlined caller, the
// inlining budget of GCC going to it.
AERONET_NOINLINE unsigned ObsoleteMonthOfAbbreviation(const char* ptr) { return MonthOfAbbreviation(ptr); }
AERONET_NOINLINE unsigned ObsoleteWeekdayOfAbbreviation(const char* ptr) { return WeekdayOfAbbreviation(ptr); }

// The time point of a date from its fields, whatever their range: years 0 to 9999 fit in seconds, while SysTimePoint
// (nanoseconds) only covers 1678 to 2262 (see SaturatedSysTimePoint()). The sum starts from sys_seconds: days, hours
// and minutes have an int representation on MSVC, and a count of minutes overflows it past the year 6053.
SysTimePoint SaturatedDate(std::chrono::sys_days dayPoint, int hourValue, int minuteValue, int secondValue) {
  const std::chrono::sys_seconds date = std::chrono::sys_seconds{dayPoint} + std::chrono::hours{hourValue} +
                                        std::chrono::minutes{minuteValue} + std::chrono::seconds{secondValue};
  return SaturatedSysTimePoint(date.time_since_epoch().count());
}

// Reads "hh:mm:ss" at 'timeOfDay' (separators and digits checked by the caller) and makes the date of an obsolete
// format, kInvalidTimePoint when its fields are out of range, or when 'weekday' (c_encoding(), 7 when unknown) is not
// the day of the week of the date.
SysTimePoint MakeObsoleteDate(int year, unsigned month, unsigned day, const char* timeOfDay, unsigned weekday) {
  const int hourValue = read2(timeOfDay);
  const int minuteValue = read2(timeOfDay + 3);
  const int secondValue = read2(timeOfDay + 6);
  if (month == 0 || hourValue > 23 || minuteValue > 59 || secondValue > 60) {
    return kInvalidTimePoint;
  }

  const std::chrono::year_month_day ymd{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
  if (!ymd.ok()) {
    return kInvalidTimePoint;
  }

  const std::chrono::sys_days dayPoint{ymd};
  if (weekday != std::chrono::weekday{dayPoint}.c_encoding()) {
    return kInvalidTimePoint;
  }
  return SaturatedDate(dayPoint, hourValue, minuteValue, secondValue);
}

// rfc850-date (obsolete): "Sunday, 06-Nov-94 08:49:37 GMT", the day name being written in full.
SysTimePoint ParseRfc850Date(const char* ptr, std::size_t len) {
  const std::string_view dayName(ptr, len - kRfc850DateTailLen);
  const char* tail = ptr + dayName.size();
  if (tail[0] != ',' || tail[1] != ' ' || tail[4] != '-' || tail[8] != '-' || tail[11] != ' ' || tail[14] != ':' ||
      tail[17] != ':' || tail[20] != ' ' || tail[21] != 'G' || tail[22] != 'M' || tail[23] != 'T') {
    return kInvalidTimePoint;
  }
  if (!AreDigits(tail + 2, 2) || !AreDigits(tail + 9, 2) || !AreDigits(tail + 12, 2) || !AreDigits(tail + 15, 2) ||
      !AreDigits(tail + 18, 2)) {
    return kInvalidTimePoint;
  }

  const unsigned weekday = ObsoleteWeekdayOfAbbreviation(ptr);
  if (weekday == 7U || kWeekdayNames[weekday] != dayName) {
    return kInvalidTimePoint;
  }
  const unsigned day = read2(tail + 2);
  if (day == 0) {
    return kInvalidTimePoint;
  }

  // RFC 9110 §5.6.7: a two-digit year more than 50 years in the future is the most recent past year with these digits.
  const int currentYear =
      static_cast<int>(std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(SysClock::now())}.year());
  int year = currentYear - (currentYear % 100) + read2(tail + 9);
  if (year > currentYear + 50) {
    year -= 100;
  }
  return MakeObsoleteDate(year, ObsoleteMonthOfAbbreviation(tail + 5), day, tail + 12, weekday);
}

// asctime-date (obsolete): "Sun Nov  6 08:49:37 1994", the day of the month padded with a space.
SysTimePoint ParseAsctimeDate(const char* ptr) {
  if (ptr[3] != ' ' || ptr[7] != ' ' || ptr[10] != ' ' || ptr[13] != ':' || ptr[16] != ':' || ptr[19] != ' ') {
    return kInvalidTimePoint;
  }
  if ((ptr[8] != ' ' && !isdigit(ptr[8])) || !isdigit(ptr[9]) || !AreDigits(ptr + 11, 2) || !AreDigits(ptr + 14, 2) ||
      !AreDigits(ptr + 17, 2) || !AreDigits(ptr + 20, 4)) {
    return kInvalidTimePoint;
  }
  const unsigned day =
      (ptr[8] == ' ' ? 0U : static_cast<unsigned>(ptr[8] - '0') * 10U) + static_cast<unsigned>(ptr[9] - '0');
  if (day == 0) {
    return kInvalidTimePoint;
  }
  return MakeObsoleteDate(read4(ptr + 20), ObsoleteMonthOfAbbreviation(ptr + 4), day, ptr + 11,
                          ObsoleteWeekdayOfAbbreviation(ptr));
}

// The obsolete formats, told apart by their length (other than the one of IMF-fixdate).
AERONET_NOINLINE SysTimePoint ParseObsoleteDate(const char* ptr, std::size_t len) {
  if (len == kAsctimeDateLen) {
    return ParseAsctimeDate(ptr);
  }
  if (len >= kWeekdayNames[1].size() + kRfc850DateTailLen && len <= kWeekdayNames[3].size() + kRfc850DateTailLen) {
    return ParseRfc850Date(ptr, len);
  }
  return kInvalidTimePoint;
}

}  // namespace

SysTimePoint TryParseTimeRFC7231(const char* begPtr, const char* endPtr) {
  while (begPtr < endPtr && isspace(*begPtr)) {
    ++begPtr;
  }
  while (endPtr > begPtr && isspace(*(endPtr - 1))) {
    --endPtr;
  }

  // The three formats have distinct lengths. IMF-fixdate, the one senders generate, is parsed here, the obsolete ones
  // out of line.
  const std::size_t len = static_cast<std::size_t>(endPtr - begPtr);
  if (len != RFC7231DateStrLen) [[unlikely]] {
    return ParseObsoleteDate(begPtr, len);
  }

  // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT".
  const char* ptr = begPtr;
  if (ptr[3] != ',' || ptr[4] != ' ' || ptr[7] != ' ' || ptr[11] != ' ' || ptr[16] != ' ' || ptr[19] != ':' ||
      ptr[22] != ':' || ptr[25] != ' ') {
    return kInvalidTimePoint;
  }

  if (!isdigit(ptr[5]) || !isdigit(ptr[6]) || !isdigit(ptr[12]) || !isdigit(ptr[13]) || !isdigit(ptr[14]) ||
      !isdigit(ptr[15]) || !isdigit(ptr[17]) || !isdigit(ptr[18]) || !isdigit(ptr[20]) || !isdigit(ptr[21]) ||
      !isdigit(ptr[23]) || !isdigit(ptr[24])) {
    return kInvalidTimePoint;
  }

  // The letters are compared exactly (read3() is for digits only: "Npl" would read as "Nov").
  if (ptr[26] != 'G' || ptr[27] != 'M' || ptr[28] != 'T') {
    return kInvalidTimePoint;
  }

  const unsigned month = MonthOfAbbreviation(ptr + 8);
  if (month == 0) {
    return kInvalidTimePoint;
  }

  const int dayValue = read2(ptr + 5);
  const int yearValue = read4(ptr + 12);
  const int hourValue = read2(ptr + 17);
  const int minuteValue = read2(ptr + 20);
  const int secondValue = read2(ptr + 23);

  if (dayValue == 0 || hourValue > 23 || minuteValue > 59 || secondValue > 60) {
    return kInvalidTimePoint;
  }

  const std::chrono::year_month_day ymd{std::chrono::year{yearValue}, std::chrono::month{month},
                                        std::chrono::day{static_cast<unsigned>(dayValue)}};
  if (!ymd.ok()) {
    return kInvalidTimePoint;
  }

  const std::chrono::sys_days dayPoint{ymd};
  // The weekday token (e.g. "Sun") must be the weekday of the date: an unknown one has the index 7, never equal.
  if (WeekdayOfAbbreviation(ptr) != std::chrono::weekday{dayPoint}.c_encoding()) {
    return kInvalidTimePoint;
  }

  return SaturatedDate(dayPoint, hourValue, minuteValue, secondValue);
}

}  // namespace aeronet
