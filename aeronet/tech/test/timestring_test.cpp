#include "aeronet/timestring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "aeronet/cctype.hpp"
#include "aeronet/format-lite.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

using namespace std::chrono;

namespace {

constexpr bool IsDate(CivilDate date, int yearValue, unsigned monthValue, unsigned dayValue) {
  return date.year == yearValue && date.month == monthValue && date.day == dayValue;
}

// Known days, and the limits of the ranges of CivilDateOfDays() and DaysOfCivilDate(), beyond the years of
// std::chrono::year.
static_assert(IsDate(CivilDateOfDays(0), 1970, 1, 1) && DaysOfCivilDate({1970, 1, 1}) == 0 && WeekdayOfDays(0) == 4);
static_assert(IsDate(CivilDateOfDays(-1), 1969, 12, 31) && IsDate(CivilDateOfDays(11016), 2000, 2, 29));
static_assert(IsDate(CivilDateOfDays(-536895458), -1468000, 3, 1) && IsDate(CivilDateOfDays(536846365), 1471805, 6, 5));
static_assert(DaysOfCivilDate({-1468000, 3, 1}) == -536895458 && DaysOfCivilDate({1471745, 2, 28}) == 536824354);

}  // namespace

// The calendar of timestring.hpp against the one of <chrono>, for each day of the years 0 to 9999.
TEST(CivilDateTest, SameAsStdChronoForEachDay) {
  const std::int32_t firstDay = sys_days{year{0} / 1 / 1}.time_since_epoch().count();
  const std::int32_t endDay = sys_days{year{10000} / 1 / 1}.time_since_epoch().count();
  std::int64_t nbMismatches = 0;
  for (std::int32_t epochDay = firstDay; epochDay < endDay; ++epochDay) {
    const sys_days dayPoint{days{epochDay}};
    const year_month_day expected{dayPoint};
    const CivilDate date = CivilDateOfDays(epochDay);
    if (date.year != static_cast<int>(expected.year()) || date.month != static_cast<unsigned>(expected.month()) ||
        date.day != static_cast<unsigned>(expected.day()) || DaysOfCivilDate(date) != epochDay ||
        !IsValidCivilDate(date) || WeekdayOfDays(epochDay) != weekday{dayPoint}.c_encoding()) {
      ADD_FAILURE() << "day " << epochDay;
      if (++nbMismatches == 10) {
        break;
      }
    }
  }
}

TEST(CivilDateTest, ValidityAndLeapYearsAsStdChrono) {
  for (int yearValue = -1000; yearValue <= 10000; ++yearValue) {
    ASSERT_EQ(IsLeapYear(yearValue), year{yearValue}.is_leap()) << yearValue;
  }
  for (const int yearValue : {0, 1900, 2000, 2023, 2024, 2100, 9999}) {
    for (unsigned monthValue = 0; monthValue <= 13; ++monthValue) {
      for (unsigned dayValue = 0; dayValue <= 32; ++dayValue) {
        EXPECT_EQ(IsValidCivilDate({yearValue, monthValue, dayValue}),
                  (year_month_day{year{yearValue}, month{monthValue}, day{dayValue}}.ok()))
            << yearValue << '-' << monthValue << '-' << dayValue;
      }
    }
  }
}

TEST(CivilDateTest, WeekdayOfDaysBeforeTheEpoch) {
  EXPECT_EQ(WeekdayOfDays(-1), 3U);  // 1969-12-31, a Wednesday
  EXPECT_EQ(WeekdayOfDays(-4), 0U);  // 1969-12-28, a Sunday
  EXPECT_EQ(WeekdayOfDays(-5), 6U);  // 1969-12-27, a Saturday
}

TEST(TimeStringIso8601UTCTest, BasicIso8601Format) {
  char buf[ISO8601UTCWithMsStrLen];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14} + std::chrono::hours{12} +
                    std::chrono::minutes{34} + std::chrono::seconds{56} + std::chrono::milliseconds{789};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2025-08-14T12:34:56.789Z");
}

TEST(TimeStringIso8601UTCTest, Midnight) {
  char buf[ISO8601UTCWithMsStrLen];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2022} / 1 / 1};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2022-01-01T00:00:00.000Z");
}

TEST(TimeStringIso8601UTCTest, EndOfYear) {
  char buf[ISO8601UTCWithMsStrLen];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2023} / 12 / 31} + std::chrono::hours{23} +
                    std::chrono::minutes{59} + std::chrono::seconds{59} + std::chrono::milliseconds{999};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2023-12-31T23:59:59.999Z");
}

TEST(TimeStringIso8601UTCTest, LeapYearFeb29) {
  char buf[ISO8601UTCWithMsStrLen];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2024} / 2 / 29} + std::chrono::hours{6} +
                    std::chrono::minutes{30} + std::chrono::seconds{15} + std::chrono::milliseconds{123};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2024-02-29T06:30:15.123Z");
}

TEST(TimeStringIso8601UTCTest, SingleDigitMonthDay) {
  char buf[ISO8601UTCWithMsStrLen];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 3 / 7} + std::chrono::hours{1} +
                    std::chrono::minutes{2} + std::chrono::seconds{3} + std::chrono::milliseconds{4};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2025-03-07T01:02:03.004Z");
}

TEST(TimeStringIso8601UTCTest, ZeroMilliseconds) {
  char buf[ISO8601UTCWithMsStrLen];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14} + std::chrono::hours{12} +
                    std::chrono::minutes{34} + std::chrono::seconds{56};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2025-08-14T12:34:56.000Z");
}

TEST(TimeStringIso8601UTCTest, MaximumMilliseconds) {
  char buf[ISO8601UTCWithMsStrLen];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14} + std::chrono::hours{23} +
                    std::chrono::minutes{59} + std::chrono::seconds{59} + std::chrono::milliseconds{999};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2025-08-14T23:59:59.999Z");
}

TEST(TimeStringIso8601UTCTest, MinimumDate) {
  char buf[ISO8601UTCWithMsStrLen];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{1970} / 1 / 1};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "1970-01-01T00:00:00.000Z");
}

TEST(TimeStringIso8601UTCTest, NegativeMilliseconds) {
  char buf[ISO8601UTCWithMsStrLen];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14} + std::chrono::hours{12} +
                    std::chrono::minutes{34} + std::chrono::seconds{56} - std::chrono::milliseconds{1};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  // Should roll back to previous second
  EXPECT_EQ(std::string_view(buf, end), "2025-08-14T12:34:55.999Z");
}

TEST(DateIso8601UTCTest, BasicDate) {
  char buf[16];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14};
  char* end = DateISO8601UTC(tp, buf);
  std::string_view out(buf, end);
  EXPECT_EQ(out, "2025-08-14");
  EXPECT_EQ(end - buf, 10);  // pointer advancement
}

TEST(DateIso8601UTCTest, LeapDay) {
  char buf[16];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2024} / 2 / 29};
  char* end = DateISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2024-02-29");
}

TEST(DateIso8601UTCTest, SingleDigitMonthDayPadding) {
  char buf[16];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 3 / 7};
  char* end = DateISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2025-03-07");  // zero padded
}

TEST(DateIso8601UTCTest, MinimumSupportedEpoch) {
  char buf[16];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{1970} / 1 / 1};
  char* end = DateISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "1970-01-01");
}

// ------------------------ RFC7231 parsing tests ------------------------
TEST(TimeStringRFC7231Test, RoundTrip) {
  using namespace std::chrono;
  SysTimePoint tp = sys_days{year{2025} / 8 / 14} + hours{12} + minutes{34} + seconds{56};
  char buf[64];
  char* end = TimeToStringRFC7231(tp, buf);
  std::string_view sv(buf, end);
  auto parsed = TryParseTimeRFC7231(sv);
  EXPECT_NE(parsed, kInvalidTimePoint);
  EXPECT_EQ(time_point_cast<seconds>(parsed), time_point_cast<seconds>(tp));
}

TEST(TimeStringRFC7231Test, RoundTripWithSpaces) {
  using namespace std::chrono;
  SysTimePoint tp = sys_days{year{2025} / 8 / 14} + hours{12} + minutes{34} + seconds{56};
  char buf[64];
  buf[0] = ' ';
  buf[1] = '\t';
  char* end = TimeToStringRFC7231(tp, buf + 2);  // introduce leading spaces
  *end = ' ';
  ++end;  // trailing space
  std::string_view sv(buf, end);
  auto parsed = TryParseTimeRFC7231(sv);
  EXPECT_NE(parsed, kInvalidTimePoint);
  EXPECT_EQ(time_point_cast<seconds>(parsed), time_point_cast<seconds>(tp));
}

TEST(TimeStringRFC7231Test, ParsesKnownExample) {
  // Example from RFC: Sun, 06 Nov 1994 08:49:37 GMT
  using namespace std::chrono;
  SysTimePoint expected = sys_days{year{1994} / 11 / 6} + hours{8} + minutes{49} + seconds{37};
  auto parsed = TryParseTimeRFC7231("Sun, 06 Nov 1994 08:49:37 GMT");
  EXPECT_NE(parsed, kInvalidTimePoint);
  EXPECT_EQ(time_point_cast<seconds>(parsed), time_point_cast<seconds>(expected));
}

TEST(TimeStringRFC7231Test, RejectsMissingGMT) {
  // missing trailing GMT should fail
  auto parsed = TryParseTimeRFC7231("Sun, 06 Nov 1994 08:49:37");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, RejectsWrongWeekday) {
  // weekday that does not match date should be rejected
  auto parsed = TryParseTimeRFC7231("Mon, 06 Nov 1994 08:49:37 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, RejectsBadMonth) {
  auto parsed = TryParseTimeRFC7231("Sun, 06 Foo 1994 08:49:37 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, OnlySpaces) {
  auto parsed = TryParseTimeRFC7231("  \t \t\t");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, InvalidSeparators1) {
  auto parsed = TryParseTimeRFC7231("Mon. 01 Dec 2025 08:49:37 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, InvalidSeparators2) {
  auto parsed = TryParseTimeRFC7231("Mon,|01 Dec 2025 08:49:37 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, InvalidSeparators3) {
  auto parsed = TryParseTimeRFC7231("Mon, 01[Dec 2025 08:49:37 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, InvalidSeparators4) {
  auto parsed = TryParseTimeRFC7231("Mon, 01 Dec,2025 08:49:37 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, InvalidSeparators5) {
  auto parsed = TryParseTimeRFC7231("Mon, 01 Dec 2025j08:49:37 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, InvalidSeparators6) {
  auto parsed = TryParseTimeRFC7231("Mon, 01 Dec 2025 08;49:37 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, InvalidSeparators7) {
  auto parsed = TryParseTimeRFC7231("Mon, 01 Dec 2025 08:49'37 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, InvalidSeparators8) {
  auto parsed = TryParseTimeRFC7231("Mon, 01 Dec 2025 08:49:37-GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, ValidSeparators) {
  auto parsed = TryParseTimeRFC7231("Mon, 01 Dec 2025 08:49:37 GMT");
  EXPECT_NE(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, RejectsShortString) {
  // truncated (missing seconds) -> invalid
  auto parsed = TryParseTimeRFC7231("Sun, 06 Nov 1994 08:49 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, RejectsExtraCharacters) {
  std::string badStr = "Sun, 06 Nov 1994 08:49:37 GMT";
  badStr.push_back('x');
  auto parsed = TryParseTimeRFC7231(badStr);
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, AcceptsStringViewOverload) {
  using namespace std::chrono;
  SysTimePoint tp = sys_days{year{2025} / 12 / 25} + hours{0} + minutes{0} + seconds{0};
  char buf[64];
  char* end = TimeToStringRFC7231(tp, buf);
  std::string_view sv2(buf, end);
  auto parsed = TryParseTimeRFC7231(sv2);
  EXPECT_NE(parsed, kInvalidTimePoint);
  EXPECT_EQ(time_point_cast<seconds>(parsed), time_point_cast<seconds>(tp));
}

TEST(TimeToStringIso8601UTCFastTest, BasicDateTime) {
  char buf[20];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14} + std::chrono::hours{12} +
                    std::chrono::minutes{34} + std::chrono::seconds{56};
  char* end = TimeToStringISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2025-08-14T12:34:56Z");
  EXPECT_EQ(end - buf, 20);  // pointer advancement check
}

TEST(TimeToStringIso8601UTCFastTest, Midnight) {
  char buf[20];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2022} / 1 / 1};
  char* end = TimeToStringISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2022-01-01T00:00:00Z");
}

TEST(TimeToStringIso8601UTCFastTest, EndOfYear) {
  char buf[20];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2023} / 12 / 31} + std::chrono::hours{23} +
                    std::chrono::minutes{59} + std::chrono::seconds{59};
  char* end = TimeToStringISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2023-12-31T23:59:59Z");
}

TEST(TimeToStringIso8601UTCFastTest, LeapDay) {
  char buf[20];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2024} / 2 / 29} + std::chrono::hours{6} +
                    std::chrono::minutes{30} + std::chrono::seconds{15};
  char* end = TimeToStringISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2024-02-29T06:30:15Z");
}

TEST(TimeToStringIso8601UTCFastTest, SingleDigitComponentsPadding) {
  char buf[20];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 3 / 7} + std::chrono::hours{1} +
                    std::chrono::minutes{2} + std::chrono::seconds{3};
  char* end = TimeToStringISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "2025-03-07T01:02:03Z");
}

// ------------------------ RFC7231 (IMF-fixdate) formatting tests ------------------------

TEST(TimeToStringRFC7231Test, RfcExampleDate) {
  // RFC 7231 example: Sun, 06 Nov 1994 08:49:37 GMT
  char buf[29];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{1994} / 11 / 6} + std::chrono::hours{8} +
                    std::chrono::minutes{49} + std::chrono::seconds{37};
  char* end = TimeToStringRFC7231(tp, buf);
  std::string out(buf, end);
  EXPECT_EQ(out, "Sun, 06 Nov 1994 08:49:37 GMT");
  EXPECT_EQ(out.size(), 29U);  // IMF-fixdate length
}

TEST(TimeToStringRFC7231Test, LeapDay) {
  // 2024-02-29 is a Thursday
  char buf[29];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2024} / 2 / 29} + std::chrono::hours{6} +
                    std::chrono::minutes{30} + std::chrono::seconds{15};
  char* end = TimeToStringRFC7231(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "Thu, 29 Feb 2024 06:30:15 GMT");
}

TEST(TimeToStringRFC7231Test, SingleDigitDayAndMonthPadding) {
  // 2025-03-07 is a Friday, ensure zero padding of day
  char buf[29];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 3 / 7} + std::chrono::hours{1} +
                    std::chrono::minutes{2} + std::chrono::seconds{3};
  char* end = TimeToStringRFC7231(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "Fri, 07 Mar 2025 01:02:03 GMT");
}

TEST(TimeToStringRFC7231Test, MondayWeekdayShiftLogic) {
  // 2025-08-04 is a Monday; tests weekday mapping logic
  char buf[29];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 4} + std::chrono::hours{12} +
                    std::chrono::minutes{0} + std::chrono::seconds{0};
  char* end = TimeToStringRFC7231(tp, buf);
  EXPECT_EQ(std::string_view(buf, end), "Mon, 04 Aug 2025 12:00:00 GMT");
}

TEST(TimeToStringRFC7231Test, RoundsDownBeforeTheEpoch) {
  // As the ISO 8601 formatters: 500 ms before 1970 is the last second of 1969, not 1970-01-01.
  char buf[RFC7231DateStrLen];
  char* end = TimeToStringRFC7231(SysTimePoint{-milliseconds{500}}, buf);
  EXPECT_EQ(std::string_view(buf, end), "Wed, 31 Dec 1969 23:59:59 GMT");
  // Rounded toward 0, the second would be in the next day.
  end = TimeToStringRFC7231(SysTimePoint{-days{1} - milliseconds{1}}, buf);
  EXPECT_EQ(std::string_view(buf, end), "Tue, 30 Dec 1969 23:59:59 GMT");
}

// The extreme time points of the nanosecond SysTimePoint of libstdc++: the day floor of SysTimePoint::min() does not
// fit nanoseconds.
TEST(TimeToStringTest, ExtremeNanosecondTimePoints) {
  if constexpr (std::is_same_v<SysDuration, nanoseconds>) {
    char rfc[RFC7231DateStrLen];
    char iso[ISO8601UTCWithMsStrLen];
    EXPECT_EQ(std::string_view(rfc, TimeToStringRFC7231(SysTimePoint::min(), rfc)), "Tue, 21 Sep 1677 00:12:43 GMT");
    EXPECT_EQ(std::string_view(iso, TimeToStringISO8601UTCWithMs(SysTimePoint::min(), iso)),
              "1677-09-21T00:12:43.145Z");
    EXPECT_EQ(std::string_view(rfc, TimeToStringRFC7231(SysTimePoint::max(), rfc)), "Fri, 11 Apr 2262 23:47:16 GMT");
    EXPECT_EQ(std::string_view(iso, TimeToStringISO8601UTCWithMs(SysTimePoint::max(), iso)),
              "2262-04-11T23:47:16.854Z");
  } else {
    GTEST_SKIP() << "SysTimePoint does not count nanoseconds: its extreme years have more than 4 digits";
  }
}

// ---------------------------------------------------------------------------
// More negative RFC7231 parsing tests covering explicit invalid ranges
// ---------------------------------------------------------------------------

TEST(TimeStringRFC7231Test, RejectsNonPositiveDay) {
  // day == 0
  auto parsed = TryParseTimeRFC7231("Sun, 00 Nov 1994 08:49:37 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, RejectsHourOutOfRange) {
  // hour > 23
  auto parsed = TryParseTimeRFC7231("Sun, 06 Nov 1994 24:00:00 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, RejectsMinuteOutOfRange) {
  // minute > 59
  auto parsed = TryParseTimeRFC7231("Sun, 06 Nov 1994 08:60:00 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, RejectsSecondOutOfRange) {
  // second > 60 (only 60 allowed for leap second)
  auto parsed = TryParseTimeRFC7231("Sun, 06 Nov 1994 08:59:61 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, RejectsUnknownWeekdayToken) {
  // weekday token not one of Sun..Sat
  auto parsed = TryParseTimeRFC7231("Xxx, 06 Nov 1994 08:49:37 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

TEST(TimeStringRFC7231Test, RejectsInvalidCalendarDate) {
  // e.g., February 30 is not a valid calendar date -> ymd.ok() == false
  auto parsed = TryParseTimeRFC7231("Sun, 30 Feb 1994 08:49:37 GMT");
  EXPECT_EQ(parsed, kInvalidTimePoint);
}

// ---------------------------------------------------------------------------
// Additional RFC7231 parsing negative tests for invalid digit locations and GMT
// ---------------------------------------------------------------------------

TEST(TimeStringRFC7231Test, RejectsInvalidDigitsAtSpecificPositions) {
  // Construct a valid base string and then mutate single digit positions
  const char* base = "Sun, 06 Nov 1994 08:49:37 GMT";
  std::string str(base);

  ASSERT_NE(TryParseTimeRFC7231(str), kInvalidTimePoint);  // Sanity check: base string is valid

  // Positions that the parser expects digits (0-based index into the string):
  // ptr[5], ptr[6] -> day digits ("06")
  // ptr[12..15] -> year digits ("1994")
  // ptr[17..18] -> hour digits ("08")
  // ptr[20..21] -> minute digits ("49")
  // ptr[23..24] -> second digits ("37")
  vector<size_t> digitPositions = {5, 6, 12, 13, 14, 15, 17, 18, 20, 21, 23, 24};

  for (size_t pos : digitPositions) {
    std::string ts = str;
    // Replace a digit with a letter to force failure
    ts[pos] = 'X';
    auto parsed = TryParseTimeRFC7231(ts);
    EXPECT_EQ(parsed, kInvalidTimePoint) << "Expected failure when mutating position " << pos << " in '" << ts << "'";
  }
}

TEST(TimeStringRFC7231Test, RejectsNonGMTTimezone) {
  // Any timezone other than the literal "GMT" at ptr[26..28] should be rejected
  const char* base = "Sun, 06 Nov 1994 08:49:37 GMT";
  std::string str(base);

  ASSERT_NE(TryParseTimeRFC7231(str), kInvalidTimePoint);  // Sanity check: base string is valid

  // Try a few alternatives that are commonly seen but should be rejected by TryParseTimeRFC7231
  vector<std::string> badTz = {"UTC", "G M", "gmt", "GXT", "XYZ", "GM"};
  for (auto& tz : badTz) {
    std::string ts = str;
    // Overwrite the final 3 chars with tz (truncate/pad as necessary)
    for (size_t i = 0; i < 3; ++i) {
      ts[26 + i] = (i < tz.size()) ? tz[i] : ' ';
    }
    auto parsed = TryParseTimeRFC7231(ts);
    EXPECT_EQ(parsed, kInvalidTimePoint) << "Expected rejection for timezone variant '" << tz << "' (string: '" << ts
                                         << "')";
  }
}

TEST(TimeStringRFC7231Test, RejectsLookalikeLetterTokens) {
  // Tokens whose digit-style value (100 * c0 + 10 * c1 + c2) is the one of the expected token, which the parser once
  // compared: "Svd" for "Sun", "Npl" for "Nov", "GNJ" for "GMT".
  EXPECT_EQ(TryParseTimeRFC7231("Svd, 06 Nov 1994 08:49:37 GMT"), kInvalidTimePoint);
  EXPECT_EQ(TryParseTimeRFC7231("Sun, 06 Npl 1994 08:49:37 GMT"), kInvalidTimePoint);
  EXPECT_EQ(TryParseTimeRFC7231("Sun, 06 Nov 1994 08:49:37 GNJ"), kInvalidTimePoint);
  // Month and weekday names are case-sensitive (RFC 9110 §5.6.7).
  EXPECT_EQ(TryParseTimeRFC7231("Sun, 06 nov 1994 08:49:37 GMT"), kInvalidTimePoint);
  EXPECT_EQ(TryParseTimeRFC7231("SUN, 06 Nov 1994 08:49:37 GMT"), kInvalidTimePoint);
}

TEST(TimeStringAbbreviationsTest, MonthOfAbbreviation) {
  static_assert(MonthOfAbbreviation("Jan") == 1U);
  static_assert(MonthOfAbbreviation("Dec") == 12U);
  for (unsigned monthNumber = 1; monthNumber <= 12; ++monthNumber) {
    const SysTimePoint tp = sys_days{year{2026} / month{monthNumber} / 1};
    char buf[RFC7231DateStrLen];
    TimeToStringRFC7231(tp, buf);
    EXPECT_EQ(MonthOfAbbreviation(buf + 8), monthNumber) << std::string_view(buf, sizeof(buf));
  }
  for (const char* token : {"jan", "JAN", "Jbd", "Npl", "Foo", "Ja\0"}) {
    EXPECT_EQ(MonthOfAbbreviation(token), 0U) << token;
  }
}

TEST(TimeStringAbbreviationsTest, WeekdayOfAbbreviation) {
  static_assert(WeekdayOfAbbreviation("Sun") == 0U);
  static_assert(WeekdayOfAbbreviation("Sat") == 6U);
  for (unsigned dayNumber = 1; dayNumber <= 7; ++dayNumber) {
    const SysTimePoint tp = sys_days{year{2026} / November / day{dayNumber}};
    char buf[RFC7231DateStrLen];
    TimeToStringRFC7231(tp, buf);
    EXPECT_EQ(WeekdayOfAbbreviation(buf), weekday{sys_days{year{2026} / November / day{dayNumber}}}.c_encoding())
        << std::string_view(buf, sizeof(buf));
  }
  for (const char* token : {"Jan", "sun", "SUN", "Svd", "Sum", "\xFF\xFF\xFF"}) {
    EXPECT_EQ(WeekdayOfAbbreviation(token), 7U) << token;
  }
}

TEST(TimeStringAbbreviationsTest, OnlyTheAbbreviationsMatch) {
  // Every 3 letters (and some other bytes) that hash to a used slot are compared to the abbreviation of the slot.
  unsigned nbMonths = 0;
  unsigned nbWeekdays = 0;
  for (unsigned ch0 = 0x20; ch0 < 0x80; ++ch0) {
    for (unsigned ch1 = 0x20; ch1 < 0x80; ++ch1) {
      for (unsigned ch2 = 0x20; ch2 < 0x80; ++ch2) {
        const char token[]{static_cast<char>(ch0), static_cast<char>(ch1), static_cast<char>(ch2)};
        nbMonths += MonthOfAbbreviation(token) != 0U ? 1U : 0U;
        nbWeekdays += WeekdayOfAbbreviation(token) != 7U ? 1U : 0U;
      }
    }
  }
  EXPECT_EQ(nbMonths, 12U);
  EXPECT_EQ(nbWeekdays, 7U);
}

// RFC 9110 §5.6.7: a recipient accepts the obsolete rfc850-date and asctime-date formats.
TEST(TimeStringRFC7231Test, ParsesObsoleteFormats) {
  const SysTimePoint expected = sys_days{year{1994} / 11 / 6} + hours{8} + minutes{49} + seconds{37};
  EXPECT_EQ(TryParseTimeRFC7231("Sunday, 06-Nov-94 08:49:37 GMT"), expected);
  EXPECT_EQ(TryParseTimeRFC7231(" Sunday, 06-Nov-94 08:49:37 GMT\t"), expected);
  EXPECT_EQ(TryParseTimeRFC7231("Sun Nov  6 08:49:37 1994"), expected);
  const SysTimePoint expected16 = sys_days{year{1994} / 11 / 16} + hours{8} + minutes{49} + seconds{37};
  EXPECT_EQ(TryParseTimeRFC7231("Wednesday, 16-Nov-94 08:49:37 GMT"), expected16);
  EXPECT_EQ(TryParseTimeRFC7231("Wed Nov 16 08:49:37 1994"), expected16);
}

// RFC 9110 §5.6.7: a two-digit year more than 50 years in the future is in the past century.
TEST(TimeStringRFC7231Test, Rfc850TwoDigitYear) {
  static constexpr std::array<std::string_view, 7> kDayNames{
      "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
  };
  const int currentYear = static_cast<int>(year_month_day{floor<days>(SysClock::now())}.year());
  for (const int yearOffset : {-30, 0, 20, 50, 51, 70}) {
    const int expectedYear = currentYear + yearOffset - (yearOffset > 50 ? 100 : 0);
    const sys_days day{year{expectedYear} / 3 / 1};
    const std::string date =
        Format("{}, 01-Mar-{:02} 00:00:00 GMT", kDayNames[weekday{day}.c_encoding()], expectedYear % 100);
    SCOPED_TRACE(date);
    EXPECT_EQ(TryParseTimeRFC7231(date), SysTimePoint{day});
  }
}

TEST(TimeStringRFC7231Test, RejectsMalformedObsoleteFormats) {
  for (std::string_view date : {
           "Sunday, 06-Nov-94 08:49:37 UTC",   // not GMT
           "Sundae, 06-Nov-94 08:49:37 GMT",   // unknown day name
           "Monday, 06-Nov-94 08:49:37 GMT",   // wrong day of the week
           "Sun, 06-Nov-94 08:49:37 GMT   x",  // abbreviated day name, garbage
           "Sunday, 06 Nov 94 08:49:37 GMT",   // separators
           "Sunday, 0x-Nov-94 08:49:37 GMT",   // digits
           "Sunday, 00-Nov-94 08:49:37 GMT",   // day 0
           "Sunday, 06-Nvo-94 08:49:37 GMT",   // month
           "Sunday, 06-Nov-94 24:49:37 GMT",   // hour
           "Sunday, 06-Nov-94 08:60:37 GMT",   // minute
           "Sunday, 06-Nov-94 08:49:61 GMT",   // second
           "Sunday, 06-Nov-94 08:49:37 GXT",   // GMT
           "Sunday, 06-Nov-94 08:49:37 GMX",   // GMT
           "Sun, 06 Nov 1994 08:49:37 GXT",    // GMT of IMF-fixdate
           "Sun, 06 Nov 1994 08:49:37 GMX",    // GMT of IMF-fixdate
           "Sun,Nov  6 08:49:37 1994",         // separators
           "Sun Nov x6 08:49:37 1994",         // digits
           "Sun Nov  6 08:49:37 19x4",         // year digits
           "Sun Nov  0 08:49:37 1994",         // day 0
           "Mon Nov  6 08:49:37 1994",         // wrong day of the week
           "Sun Nov 31 08:49:37 1994",         // no such day
           "Sunday, 06-Nov-94 08:49:37",       // length of no format
       }) {
    SCOPED_TRACE(date);
    EXPECT_EQ(TryParseTimeRFC7231(date), kInvalidTimePoint);
  }
}

// Every separator and digit of the obsolete formats is checked: changing any of them makes the date invalid.
TEST(TimeStringRFC7231Test, RejectsEachMalformedCharOfObsoleteFormats) {
  for (std::string_view valid : {"Sunday, 06-Nov-94 08:49:37 GMT", "Wed Nov 16 08:49:37 1994"}) {
    ASSERT_NE(TryParseTimeRFC7231(valid), kInvalidTimePoint);
    for (std::size_t pos = 0; pos < valid.size(); ++pos) {
      const char orig = valid[pos];
      if (orig != ',' && orig != ' ' && orig != '-' && orig != ':' && static_cast<int>(isdigit(orig)) == 0) {
        continue;  // letters: day names, months and GMT are covered by RejectsMalformedObsoleteFormats
      }
      std::string mutated(valid);
      mutated[pos] = 'x';
      SCOPED_TRACE(mutated);
      EXPECT_EQ(TryParseTimeRFC7231(mutated), kInvalidTimePoint);
    }
  }
  // An unknown day name, and a length between the rfc850-date ones and no format.
  EXPECT_EQ(TryParseTimeRFC7231("Xyzday, 06-Nov-94 08:49:37 GMT"), kInvalidTimePoint);
  EXPECT_EQ(TryParseTimeRFC7231("Sunday, 06-Nov-94 08:49:37 GMT 08:49"), kInvalidTimePoint);
}

// A date beyond the range of SysTimePoint (nanoseconds: 1678 to 2262) saturates instead of overflowing.
TEST(TimeStringRFC7231Test, DatesBeyondClockRangeSaturate) {
  const SysTimePoint maxDate = TryParseTimeRFC7231("Fri, 31 Dec 9999 23:59:59 GMT");
  const SysTimePoint minDate = TryParseTimeRFC7231("Sat, 01 Jan 0000 00:00:00 GMT");
  ASSERT_NE(maxDate, kInvalidTimePoint);
  ASSERT_NE(minDate, kInvalidTimePoint);
  EXPECT_GT(maxDate, SysTimePoint{sys_days{year{2262} / 1 / 1}});
  EXPECT_LT(minDate, SysTimePoint{sys_days{year{1678} / 1 / 1}});
}

TEST(SaturatedSysTimePointTest, ClampsToTheClockRange) {
  EXPECT_EQ(SaturatedSysTimePoint(0), SysTimePoint{});
  // Sub-second parts in multiples of 100 ns, the tick of system_clock on Windows.
  EXPECT_EQ(SaturatedSysTimePoint(-1, 999'999'900), SysTimePoint{} - duration_cast<SysDuration>(nanoseconds{100}));
  EXPECT_EQ(SaturatedSysTimePoint(1'700'000'000, 500),
            SysTimePoint{seconds{1'700'000'000}} + duration_cast<SysDuration>(nanoseconds{500}));
  const SysTimePoint maxPoint = SaturatedSysTimePoint(std::numeric_limits<std::int64_t>::max(), 999'999'999);
  const SysTimePoint minPoint = SaturatedSysTimePoint(std::numeric_limits<std::int64_t>::min());
  EXPECT_NE(maxPoint, kInvalidTimePoint);
  EXPECT_GT(maxPoint, SysTimePoint::max() - seconds{2});
  EXPECT_LT(minPoint, SysTimePoint::min() + seconds{1});
  static_assert(SaturatedSysTimePoint(1) == SysTimePoint{seconds{1}});
}

}  // namespace aeronet
