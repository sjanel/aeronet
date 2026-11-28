#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "aeronet/chrono-lite.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

// A date of the proleptic Gregorian calendar, as std::chrono::year_month_day, which needs the whole <chrono>.
struct CivilDate {
  int year;
  unsigned month;  // 1 to 12
  unsigned day;    // 1 to 31
};

/// The date of the day 'daysSinceEpoch' (0 for 1970-01-01), as std::chrono::year_month_day{sys_days}: the algorithm of
/// Neri and Schneider (https://arxiv.org/abs/2102.06959, proposition 6.3), that of libstdc++. 'daysSinceEpoch' must be
/// in [-536895458, 536846365] (-1468000-03-01 to 1471805-06-05), which holds the days of any SysTimePoint.
constexpr CivilDate CivilDateOfDays(std::int64_t daysSinceEpoch) noexcept {
  static constexpr auto kYearShift = static_cast<std::uint32_t>(-1468000);
  static constexpr std::uint32_t kDayShift = 536895458;

  // Out of the range, the 32-bit arithmetic below wraps: 'shiftedDays' must not be negative, nor 4 * shiftedDays + 3
  // overflow.
  assert(daysSinceEpoch >= -std::int64_t{kDayShift} &&
         // NOLINTNEXTLINE(modernize-use-integer-sign-comparison)
         daysSinceEpoch < static_cast<std::int64_t>((1UL << 30U) - kDayShift));
  const std::uint32_t shiftedDays = static_cast<std::uint32_t>(daysSinceEpoch) + kDayShift;
  const std::uint32_t n1 = (4U * shiftedDays) + 3U;
  const std::uint32_t centuries = n1 / 146097U;
  const std::uint32_t dayOfCentury = n1 % 146097U / 4U;

  const std::uint32_t n2 = (4U * dayOfCentury) + 3U;
  const std::uint64_t u2 = std::uint64_t{2939745} * n2;
  const auto yearOfCentury = static_cast<std::uint32_t>(u2 >> 32U);
  const std::uint32_t dayOfYear = static_cast<std::uint32_t>(u2 & 0xFFFFFFFFU) / 2939745U / 4U;

  const std::uint32_t n3 = (2141U * dayOfYear) + 197913U;
  const std::uint32_t month = n3 >> 16U;
  const std::uint32_t dayOfMonth = (n3 & 0xFFFFU) / 2141U;

  // The computation counts the years from March: January and February belong to the next year.
  const bool janOrFeb = dayOfYear >= 306U;
  return {
      static_cast<int>((100U * centuries) + yearOfCentury + (janOrFeb ? 1U : 0U) + kYearShift),
      janOrFeb ? month - 12U : month,
      dayOfMonth + 1U,
  };
}

/// The day of 'date' since 1970-01-01, as sys_days{year_month_day}: the algorithm of Neri and Schneider (proposition
/// 6.2), that of libstdc++. 'date' must be valid (see IsValidCivilDate()) and from -1468000-03-01 to 1471745-02-28.
constexpr std::int32_t DaysOfCivilDate(CivilDate date) noexcept {
  constexpr auto kYearShift = static_cast<std::uint32_t>(-1468000);
  constexpr std::uint32_t kDayShift = 536895458;

  // The computation counts the years from March: January and February belong to the previous year.
  const bool janOrFeb = date.month < 3U;
  const std::uint32_t year = static_cast<std::uint32_t>(date.year) - kYearShift - (janOrFeb ? 1U : 0U);
  const std::uint32_t month = janOrFeb ? date.month + 12U : date.month;
  // Out of the range, 'year' wraps below 0 or 1461 * year overflows.
  assert(year <= 2939744U);

  const std::uint32_t century = year / 100U;
  const std::uint32_t yearDays = (1461U * year / 4U) - century + (century / 4U);
  const std::uint32_t monthDays = ((979U * month) - 2919U) / 32U;

  return static_cast<std::int32_t>(yearDays + monthDays + (date.day - 1U) - kDayShift);
}

/// Whether 'year' is a leap year of the proleptic Gregorian calendar.
constexpr bool IsLeapYear(int year) noexcept {
  // year % 100 == 0 ? year % 400 == 0 : year % 4 == 0, as libstdc++: a multiple of 4 is a multiple of 100 if and only
  // if it is one of 25, and a multiple of 400 if and only if it is one of 16.
  return (static_cast<unsigned>(year) & (year % 25 == 0 ? 15U : 3U)) == 0;
}

/// Whether 'date' exists (month 1 to 12, day 1 to the last of the month), as std::chrono::year_month_day::ok() but for
/// any year (ok() also requires -32767 to 32767).
constexpr bool IsValidCivilDate(CivilDate date) noexcept {
  if (date.month == 0 || date.month > 12U || date.day == 0) {
    return false;
  }
  if (date.month == 2U) {
    return date.day <= (IsLeapYear(date.year) ? 29U : 28U);
  }
  // 31 days, except April, June, September and November (30).
  return date.day <= (((date.month ^ (date.month >> 3U)) & 1U) | 30U);
}

/// The day of the week of the day 'daysSinceEpoch', 0 for Sunday, as std::chrono::weekday{sys_days}.c_encoding().
constexpr unsigned WeekdayOfDays(std::int64_t daysSinceEpoch) noexcept {
  // (4 + daysSinceEpoch) mod 7 (1970-01-01 was a Thursday) in unsigned arithmetic, as libstdc++: a negative day is
  // shifted by kNegativeShift, which leaves its remainder unchanged and its sum with 4 positive.
  constexpr std::uint64_t kMax = ~std::uint64_t{0};
  constexpr std::uint64_t kNegativeShift = kMax - ((7U * ((kMax - 260U) / 7U)) - 1U);
  const std::uint64_t shift = daysSinceEpoch >= 0 ? 0U : kNegativeShift;
  return static_cast<unsigned>((4U + static_cast<std::uint64_t>(daysSinceEpoch) - shift) % 7U);
}

// English abbreviations of the week days from Sunday (the order of std::chrono::weekday::c_encoding()), and of the
// months from January, as HTTP dates spell them (RFC 9110 §5.6.7). Fixed-width: no pointer-array indirection.
inline constexpr char kWeekdayAbbreviations[][3]{
    {'S', 'u', 'n'}, {'M', 'o', 'n'}, {'T', 'u', 'e'}, {'W', 'e', 'd'},
    {'T', 'h', 'u'}, {'F', 'r', 'i'}, {'S', 'a', 't'},
};

inline constexpr char kMonthAbbreviations[][3]{
    {'J', 'a', 'n'}, {'F', 'e', 'b'}, {'M', 'a', 'r'}, {'A', 'p', 'r'}, {'M', 'a', 'y'}, {'J', 'u', 'n'},
    {'J', 'u', 'l'}, {'A', 'u', 'g'}, {'S', 'e', 'p'}, {'O', 'c', 't'}, {'N', 'o', 'v'}, {'D', 'e', 'c'},
};

namespace detail {

// The 3 chars at 'ptr' in one integer, to compare a 3-letter abbreviation at once.
constexpr std::uint32_t Pack3(const char* ptr) noexcept {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(ptr[0])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(ptr[1])) << 8U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(ptr[2])) << 16U);
}

// Lookup of N 3-letter abbreviations by a perfect hash: the slot ((packed * Multiplier) >> Shift) of 2^Bits is a
// different one for each abbreviation (see perfect()), so that a lookup costs a multiply, a table load and a compare
// whatever the input, where a linear search costs up to N mispredicted branches for unpredictable dates.
template <std::size_t N, std::uint32_t Multiplier, unsigned Shift, unsigned Bits>
class AbbreviationTable {
 public:
  explicit constexpr AbbreviationTable(const char (&abbreviations)[N][3]) noexcept {
    for (std::uint8_t& idx : _indexOfSlot) {
      idx = N;
    }
    for (std::size_t idx = 0; idx < N; ++idx) {
      _packed[idx] = Pack3(abbreviations[idx]);
      _indexOfSlot[Slot(_packed[idx])] = static_cast<std::uint8_t>(idx);
    }
    _packed[N] = ~std::uint32_t{0};  // matches no 3 chars: the result of an empty slot
  }

  // Whether each abbreviation has a slot of its own, which Multiplier and Shift must ensure.
  [[nodiscard]] consteval bool perfect() const noexcept {
    std::size_t nbUsedSlots = 0;
    for (const std::uint8_t idx : _indexOfSlot) {
      nbUsedSlots += idx != N ? 1U : 0U;
    }
    return nbUsedSlots == N;
  }

  // Index of the abbreviation (case-sensitive) that is the 3 chars at 'ptr', N when they are none.
  [[nodiscard]] constexpr std::size_t index(const char* ptr) const noexcept {
    const std::uint32_t packed = Pack3(ptr);
    const std::uint8_t idx = _indexOfSlot[Slot(packed)];
    return _packed[idx] == packed ? idx : N;
  }

 private:
  static constexpr std::size_t Slot(std::uint32_t packed) noexcept {
    return ((packed * Multiplier) >> Shift) & ((1U << Bits) - 1U);
  }

  std::uint32_t _packed[N + 1]{};
  std::uint8_t _indexOfSlot[1U << Bits]{};
};

inline constexpr AbbreviationTable<7, 62, 20, 3> kWeekdayTable(kWeekdayAbbreviations);
inline constexpr AbbreviationTable<12, 122, 16, 4> kMonthTable(kMonthAbbreviations);

static_assert(kWeekdayTable.perfect() && kMonthTable.perfect());

}  // namespace detail

/// The day of the week, as std::chrono::weekday::c_encoding() (0 for Sunday), whose abbreviation ("Sun" to "Sat",
/// case-sensitive) is the 3 chars at 'ptr'. 7 when there is none.
constexpr unsigned WeekdayOfAbbreviation(const char* ptr) noexcept {
  return static_cast<unsigned>(detail::kWeekdayTable.index(ptr));
}

/// The month (1 to 12) whose abbreviation ("Jan" to "Dec", case-sensitive) is the 3 chars at 'ptr', 0 when there is
/// none.
constexpr unsigned MonthOfAbbreviation(const char* ptr) noexcept {
  const std::size_t idx = detail::kMonthTable.index(ptr);
  return idx == 12U ? 0U : static_cast<unsigned>(idx) + 1U;
}

namespace detail {

// The day since 1970-01-01 of 'timePoint', and its milliseconds since the start of that day.
struct DayAndMillisecond {
  std::int64_t epochDay;
  std::uint32_t millisecondOfDay;
};

constexpr DayAndMillisecond DayAndMillisecondOf(SysTimePoint timePoint) noexcept {
  // In milliseconds before taking the day: the day floor of a time point near SysTimePoint::min() does not fit its
  // ticks (nanoseconds for libstdc++), that of its milliseconds does.
  const auto msFloor = std::chrono::floor<std::chrono::milliseconds>(timePoint);
  const auto dayFloor = std::chrono::floor<std::chrono::days>(msFloor);
  return {
      dayFloor.time_since_epoch().count(),
      static_cast<std::uint32_t>((msFloor - dayFloor).count()),
  };
}

// Writes 'YYYY-MM-DD' and returns a pointer after it.
constexpr auto WriteDateISO8601(CivilDate date, auto out) {
  out = write4(out, date.year);
  *out = '-';
  out = write2(++out, date.month);
  *out = '-';
  return write2(++out, date.day);
}

// Writes 'YYYY-MM-DDTHH:MM:SS' and returns a pointer after it.
constexpr auto WriteDateTimeISO8601(DayAndMillisecond dayAndMs, auto out) {
  const std::uint32_t secondOfDay = dayAndMs.millisecondOfDay / 1000U;
  const std::uint32_t hour = secondOfDay / 3600U;
  const std::uint32_t secondOfHour = secondOfDay - (hour * 3600U);
  const std::uint32_t minute = secondOfHour / 60U;
  out = WriteDateISO8601(CivilDateOfDays(dayAndMs.epochDay), out);
  *out = 'T';
  out = write2(++out, hour);
  *out = ':';
  out = write2(++out, minute);
  *out = ':';
  return write2(++out, secondOfHour - (minute * 60U));
}

}  // namespace detail

// The formatters below write 4-digit years: the year of their time point must be 0 to 9999, as that of any SysTimePoint
// of libstdc++ (1677 to 2262), but not of all those of libc++ (microseconds) and MSVC (100 ns), which span far more.

/// Writes chars of the representation of a given time point in ISO 8601 UTC format with maximum performance and return
/// a pointer after the last char written. The written format will be:
///   - 'YYYY-MM-DD'
/// The buffer should have a space of at least 10 chars.
constexpr auto DateISO8601UTC(SysTimePoint timePoint, auto out) {
  return detail::WriteDateISO8601(
      CivilDateOfDays(std::chrono::floor<std::chrono::days>(timePoint).time_since_epoch().count()), out);
}

/// Writes chars of the representation of a given time point in ISO 8601 UTC format with maximum performance and return
/// a pointer after the last char written. The written format will be:
///   - 'YYYY-MM-DDTHH:MM:SSZ'
/// The buffer should have a space of at least 20 chars.
template <bool WithFinalZ = true>
constexpr auto TimeToStringISO8601UTC(SysTimePoint timePoint, auto out) {
  out = detail::WriteDateTimeISO8601(detail::DayAndMillisecondOf(timePoint), out);
  if constexpr (WithFinalZ) {
    *out = 'Z';
    ++out;
  }
  return out;
}

/// Writes chars of the representation of a given time point in ISO 8601 UTC format with maximum performance and return
/// a pointer after the last char written. The written format will be (in millisecond precision):
///   - 'YYYY-MM-DDTHH:MM:SS.sssZ'
/// The buffer should have a space of at least 24 chars (ISO8601UTCWithMsStrLen).
constexpr auto TimeToStringISO8601UTCWithMs(SysTimePoint timePoint, auto out) {
  const detail::DayAndMillisecond dayAndMs = detail::DayAndMillisecondOf(timePoint);
  out = detail::WriteDateTimeISO8601(dayAndMs, out);
  *out = '.';
  out = write3(++out, dayAndMs.millisecondOfDay % 1000U);
  *out = 'Z';
  return ++out;
}

/// Format a time point to an RFC7231 IMF-fixdate string (e.g. "Sun, 06 Nov 1994 08:49:37 GMT").
/// Buffer must have space for at least 29 characters (no null terminator added):
/// WWW, DD Mon YYYY HH:MM:SS GMT
/// Returns pointer past last written char.
inline auto TimeToStringRFC7231(SysTimePoint tp, auto out) {
  static constexpr char kSkeleton[]{
      '?', '?', '?', ',', ' ', '?', '?', ' ', '?', '?', '?', ' ', '?', '?', '?',
      '?', ' ', '?', '?', ':', '?', '?', ':', '?', '?', ' ', 'G', 'M', 'T',
  };

  // Rounded down, as the ISO 8601 formatters: time_point_cast() rounds toward 0, 1970-01-01 for 500 ms before it.
  const std::chrono::sys_seconds secTp = std::chrono::floor<std::chrono::seconds>(tp);
  const auto dayPoint = std::chrono::floor<std::chrono::days>(secTp);
  const std::int64_t epochDay = dayPoint.time_since_epoch().count();
  const CivilDate date = CivilDateOfDays(epochDay);

  // Seconds since midnight, in [0, 86400). Two divisions instead of three:
  // m and s are both derived from the remainder after removing hours.
  const unsigned tod = static_cast<unsigned>((secTp - dayPoint).count());
  const unsigned hour = tod / 3600;
  const unsigned rem = tod - (hour * 3600);
  const unsigned min = rem / 60;
  const unsigned sec = rem - (min * 60);

  std::memcpy(out, kSkeleton, sizeof(kSkeleton));

  std::memcpy(out, kWeekdayAbbreviations[WeekdayOfDays(epochDay)], sizeof(kWeekdayAbbreviations[0]));
  write2(out + 5, date.day);

  std::memcpy(out + 8, kMonthAbbreviations[date.month - 1], sizeof(kMonthAbbreviations[0]));

  write4(out + 12, date.year);
  write2(out + 17, hour);
  write2(out + 20, min);
  write2(out + 23, sec);

  return out + sizeof(kSkeleton);
}

// Parse an HTTP-date (RFC 9110 §5.6.7), surrounding whitespace ignored, and return its time point: IMF-fixdate
// ("Sun, 06 Nov 1994 08:49:37 GMT"), as well as the obsolete rfc850-date ("Sunday, 06-Nov-94 08:49:37 GMT", a two-digit
// year more than 50 years in the future being taken in the past century) and asctime-date ("Sun Nov  6 08:49:37
// 1994") that a recipient must accept. A date beyond the range of SysTimePoint is clamped to it (see
// SaturatedSysTimePoint()). If parsing fails, returns kInvalidTimePoint.
SysTimePoint TryParseTimeRFC7231(const char* begPtr, const char* endPtr);

// Parse an HTTP-date (see above). If parsing fails, returns kInvalidTimePoint.
inline SysTimePoint TryParseTimeRFC7231(std::string_view value) {
  return TryParseTimeRFC7231(value.data(), value.data() + value.size());
}

}  // namespace aeronet
