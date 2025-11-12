#pragma once

#include <chrono>
#include <string_view>
#include <utility>

#include "aeronet/simple-charconv.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

/// Writes chars of the representation of a given time point in ISO 8601 UTC format with maximum performance and return
/// a pointer after the last char written. The written format will be:
///   - 'YYYY-MM-DD'
/// The buffer should have a space of at least 10 chars.
constexpr auto DateISO8601UTC(SysTimePoint timePoint, auto out) {
  const auto daysFloor = std::chrono::floor<std::chrono::days>(timePoint);
  const std::chrono::year_month_day ymd{daysFloor};
  out = write4(out, static_cast<int>(ymd.year()));
  *out = '-';
  out = write2(++out, static_cast<unsigned>(ymd.month()));
  *out = '-';
  return write2(++out, static_cast<unsigned>(ymd.day()));
}

/// Writes chars of the representation of a given time point in ISO 8601 UTC format with maximum performance and return
/// a pointer after the last char written. The written format will be:
///   - 'YYYY-MM-DDTHH:MM:SSZ'
/// The buffer should have a space of at least 20 chars.
template <bool WithFinalZ = true>
constexpr auto TimeToStringISO8601UTC(SysTimePoint timePoint, auto out) {
  const auto daysFloor = std::chrono::floor<std::chrono::days>(timePoint);
  const std::chrono::hh_mm_ss hms{
      std::chrono::floor<std::chrono::milliseconds>(timePoint - daysFloor)};  // ms floor also used by ms variant
  out = DateISO8601UTC(timePoint, out);
  *out = 'T';
  out = write2(++out, hms.hours().count());
  *out = ':';
  out = write2(++out, hms.minutes().count());
  *out = ':';
  out = write2(++out, hms.seconds().count());
  if constexpr (WithFinalZ) {
    *out = 'Z';
    ++out;
  }
  return out;
}

/// Writes chars of the representation of a given time point in ISO 8601 UTC format with maximum performance and return
/// a pointer after the last char written. The written format will be (in millisecond precision):
///   - 'YYYY-MM-DDTHH:MM:SS.sssZ'
/// The buffer should have a space of at least 24 chars.
constexpr auto TimeToStringISO8601UTCWithMs(SysTimePoint timePoint, auto out) {
  const auto daysFloor = std::chrono::floor<std::chrono::days>(timePoint);
  const std::chrono::hh_mm_ss hms{std::chrono::floor<std::chrono::milliseconds>(timePoint - daysFloor)};
  out = TimeToStringISO8601UTC<false>(timePoint, out);
  *out = '.';
  out = write3(++out, hms.subseconds().count());
  *out = 'Z';
  return ++out;
}

/// Parse a string representation of a given time point in ISO 8601 UTC (RFC-3339 extended form to be more precise)
/// format with maximum performance and return a time_point. Accepted formats are the following (even without trailing
/// Z, the time will be considered UTC):
///  - YYYY
///  - YYYY-MM
///  - YYYY-MM-DD
///  - YYYY-MM-DDTHH
///  - YYYY-MM-DDTHH:MM
///  - YYYY-MM-DDTHH:MM:SS
///  - YYYY-MM-DDTHH:MM:SS.sss
///  - YYYY-MM-DDTHH:MM:SS.sssZ
///  - YYYY-MM-DDTHH:MM:SS.sss+00:00
///  - YYYY-MM-DDTHH:MM:SS.sss-05:30
/// Warning: Few checks are done on the input. It should contain at least 19 chars (up to the seconds part).
SysTimePoint StringToTimeISO8601UTC(const char* begPtr, const char* endPtr);

inline SysTimePoint StringToTimeISO8601UTC(std::string_view timeStr) {
  return StringToTimeISO8601UTC(timeStr.data(), timeStr.data() + timeStr.size());
}

/// Parse a string representation of a calendar period (duration), in the following possible formats:
///  - YYYY        (example: '2025')
///  - YYYY-MM     (example: '2025-06')
///  - YYYY-Www    (example: '2025-W22')
///  - YYYY-MM-DD  (example: '2025-06-15')
/// Returns a pair of time points [a, b) representing the start (inclusive) and end (exclusive) of the period.
std::pair<SysTimePoint, SysTimePoint> ParseTimeWindow(std::string_view str);

/// Format a time point to an RFC7231 IMF-fixdate string (e.g. "Sun, 06 Nov 1994 08:49:37 GMT").
/// Buffer must have space for at least 29 characters (no null terminator added):
/// WWW, DD Mon YYYY HH:MM:SS GMT
/// Returns pointer past last written char.
constexpr auto TimeToStringRFC7231(SysTimePoint tp, auto out) {
  using namespace std::chrono;
  static constexpr const char* const WEEKDAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static constexpr const char* const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const sys_seconds secTp = time_point_cast<seconds>(tp);
  const auto day_point = floor<days>(secTp);
  const year_month_day ymd{day_point};
  const weekday wd{day_point};
  const hh_mm_ss hms{secTp - day_point};
  out = copy3(out, WEEKDAYS[wd.c_encoding()]);
  *out = ',';
  *++out = ' ';
  out = write2(++out, static_cast<unsigned>(ymd.day()));
  *out = ' ';
  out = copy3(++out, MONTHS[static_cast<unsigned>(ymd.month()) - 1]);
  *out = ' ';
  out = write4(++out, static_cast<int>(ymd.year()));
  *out = ' ';
  out = write2(++out, hms.hours().count());
  *out = ':';
  out = write2(++out, hms.minutes().count());
  *out = ':';
  out = write2(++out, hms.seconds().count());
  *out = ' ';
  return copy3(++out, "GMT");
}

inline constexpr std::size_t kRFC7231DateStrLen = 29;
inline constexpr SysTimePoint kInvalidTimePoint = SysTimePoint::max();

// Parse a string representation of a given time point in RFC7231 IMF-fixdate format with maximum performance and
// return a time_point. If parsing fails, returns kInvalidTimePoint.
SysTimePoint TryParseTimeRFC7231(const char* begPtr, const char* endPtr);

// Parse a string representation of a given time point in RFC7231 IMF-fixdate format with maximum performance and
// return a time_point. If parsing fails, returns kInvalidTimePoint.
inline SysTimePoint TryParseTimeRFC7231(std::string_view value) {
  return TryParseTimeRFC7231(value.data(), value.data() + value.size());
}

}  // namespace aeronet
