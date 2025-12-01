#include "aeronet/timestring.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/timedef.hpp"

namespace aeronet {

using namespace std::chrono;

TEST(TimeStringIso8601UTCTest, BasicIso8601Format) {
  char buf[24];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14} + std::chrono::hours{12} +
                    std::chrono::minutes{34} + std::chrono::seconds{56} + std::chrono::milliseconds{789};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2025-08-14T12:34:56.789Z");
}

TEST(TimeStringIso8601UTCTest, Midnight) {
  char buf[24];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2022} / 1 / 1};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2022-01-01T00:00:00.000Z");
}

TEST(TimeStringIso8601UTCTest, EndOfYear) {
  char buf[24];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2023} / 12 / 31} + std::chrono::hours{23} +
                    std::chrono::minutes{59} + std::chrono::seconds{59} + std::chrono::milliseconds{999};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2023-12-31T23:59:59.999Z");
}

TEST(TimeStringIso8601UTCTest, LeapYearFeb29) {
  char buf[24];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2024} / 2 / 29} + std::chrono::hours{6} +
                    std::chrono::minutes{30} + std::chrono::seconds{15} + std::chrono::milliseconds{123};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2024-02-29T06:30:15.123Z");
}

TEST(TimeStringIso8601UTCTest, SingleDigitMonthDay) {
  char buf[24];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 3 / 7} + std::chrono::hours{1} +
                    std::chrono::minutes{2} + std::chrono::seconds{3} + std::chrono::milliseconds{4};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2025-03-07T01:02:03.004Z");
}

TEST(TimeStringIso8601UTCTest, ZeroMilliseconds) {
  char buf[24];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14} + std::chrono::hours{12} +
                    std::chrono::minutes{34} + std::chrono::seconds{56};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2025-08-14T12:34:56.000Z");
}

TEST(TimeStringIso8601UTCTest, MaximumMilliseconds) {
  char buf[24];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14} + std::chrono::hours{23} +
                    std::chrono::minutes{59} + std::chrono::seconds{59} + std::chrono::milliseconds{999};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2025-08-14T23:59:59.999Z");
}

TEST(TimeStringIso8601UTCTest, MinimumDate) {
  char buf[24];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{1970} / 1 / 1};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "1970-01-01T00:00:00.000Z");
}

TEST(TimeStringIso8601UTCTest, NegativeMilliseconds) {
  char buf[24];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14} + std::chrono::hours{12} +
                    std::chrono::minutes{34} + std::chrono::seconds{56} - std::chrono::milliseconds{1};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  // Should roll back to previous second
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2025-08-14T12:34:55.999Z");
}

TEST(TimeStringIso8601UTCTest, RoundTripConversion) {
  // ---- Added fast path tests for DateISO8601UTC ----
  char buf[24];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14} + std::chrono::hours{12} +
                    std::chrono::minutes{34} + std::chrono::seconds{56} + std::chrono::milliseconds{789};
  char* end = TimeToStringISO8601UTCWithMs(tp, buf);
  std::string_view iso(buf, static_cast<std::size_t>(end - buf));
  SysTimePoint tp2 = StringToTimeISO8601UTC(iso.data(), iso.data() + iso.size());
  char buf2[24];
  char* end2 = TimeToStringISO8601UTCWithMs(tp2, buf2);
  EXPECT_EQ(std::string_view(buf2, static_cast<std::size_t>(end2 - buf2)), iso);
}

TEST(TimeString, NoSubSecondsWithDot) {
  EXPECT_EQ(StringToTimeISO8601UTC("2025-08-14T12:34:56.Z"), StringToTimeISO8601UTC("2025-08-14T12:34:56Z"));
}

class StringToTimeISO8601UTCTest : public ::testing::Test {};

// ------------------------ Valid cases ------------------------
TEST_F(StringToTimeISO8601UTCTest, ParsesBasicISO8601UTC) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56Z");
  auto sys_days = floor<days>(tp);
  auto ymd = year_month_day(sys_days);
  EXPECT_EQ(int(ymd.year()), 2025);
  EXPECT_EQ(unsigned(ymd.month()), 8);
  EXPECT_EQ(unsigned(ymd.day()), 14);

  auto dur = tp - sys_days;

  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});
  EXPECT_EQ(duration_cast<hours>(dur).count(), 12);
  EXPECT_EQ(duration_cast<minutes>(dur).count() % 60, 34);
  EXPECT_EQ(duration_cast<seconds>(dur).count() % 60, 56);
  // ---- Added fast path tests for TimeToStringISO8601UTC (no millis) ----
}

TEST_F(StringToTimeISO8601UTCTest, ParsesISO8601UTCWithoutZ) {
  auto tp = StringToTimeISO8601UTC("2025-08-14 12:34:56");
  auto sys_days = floor<days>(tp);
  auto dur = tp - sys_days;

  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});
  EXPECT_EQ(duration_cast<hours>(dur).count(), 12);
  EXPECT_EQ(duration_cast<minutes>(dur).count() % 60, 34);
  EXPECT_EQ(duration_cast<seconds>(dur).count() % 60, 56);
}

TEST_F(StringToTimeISO8601UTCTest, ParsesWithMilliseconds) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56.123Z");
  auto sys_days = floor<days>(tp);
  auto dur = tp - sys_days;

  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});
  EXPECT_EQ(duration_cast<milliseconds>(dur).count() % 1000, 123);
}

TEST_F(StringToTimeISO8601UTCTest, ParsesWithMicroseconds) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56.123456Z");
  auto sys_days = floor<days>(tp);
  auto dur = tp - sys_days;

  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});
  EXPECT_EQ(duration_cast<microseconds>(dur).count() % 1000000, 123456);
}

TEST_F(StringToTimeISO8601UTCTest, ParsesWithNanoseconds) {
  auto tp = StringToTimeISO8601UTC("2025-08-08T18:00:00.000864693Z");
  auto sys_days = floor<days>(tp);
  auto dur = tp - sys_days;

  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});
  EXPECT_EQ(duration_cast<nanoseconds>(dur).count() % 1000000000, 864693);
}

TEST_F(StringToTimeISO8601UTCTest, ParsesWithCustomSubSecondPrecision) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56.1234567Z");
  auto sys_days = floor<days>(tp);
  auto dur = tp - sys_days;

  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});
  EXPECT_EQ(duration_cast<nanoseconds>(dur).count() % 1000000000, 123456700);
}

TEST_F(StringToTimeISO8601UTCTest, ParsesSpaceInsteadOfT) {
  auto tp = StringToTimeISO8601UTC("2025-08-14 12:34:56Z");
  auto sys_days = floor<days>(tp);
  auto dur = tp - sys_days;

  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});
  EXPECT_EQ(duration_cast<hours>(dur).count(), 12);
  EXPECT_EQ(duration_cast<minutes>(dur).count() % 60, 34);
  EXPECT_EQ(duration_cast<seconds>(dur).count() % 60, 56);
}

TEST_F(StringToTimeISO8601UTCTest, ParsesWithoutSecondsFraction) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T00:00:00Z");
  auto sys_days = floor<days>(tp);
  auto dur = tp - sys_days;

  EXPECT_GE(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});
  EXPECT_EQ(duration_cast<seconds>(dur).count(), 0);
}

// ------------------------ Edge cases ------------------------
TEST_F(StringToTimeISO8601UTCTest, ParsesStartOfMonth) {
  auto tp = StringToTimeISO8601UTC("2025-08-01T00:00:00Z");
  auto ymd = year_month_day(floor<days>(tp));
  EXPECT_EQ(unsigned(ymd.day()), 1);
}

TEST_F(StringToTimeISO8601UTCTest, ParsesEndOfYear) {
  auto tp = StringToTimeISO8601UTC("2025-12-31T23:59:59Z");
  auto ymd = year_month_day(floor<days>(tp));
  EXPECT_EQ(unsigned(ymd.month()), 12);
  EXPECT_EQ(unsigned(ymd.day()), 31);
}

// ------------------------ Invalid cases ------------------------
TEST_F(StringToTimeISO8601UTCTest, AcceptsTruncations) {
  auto tp = StringToTimeISO8601UTC("2025-08");
  auto ymd = year_month_day(floor<days>(tp));
  EXPECT_EQ(int(ymd.year()), 2025);
  EXPECT_EQ(unsigned(ymd.month()), 8);
  EXPECT_EQ(unsigned(ymd.day()), 1);

  tp = StringToTimeISO8601UTC("2025-08-14");
  ymd = year_month_day(floor<days>(tp));
  EXPECT_EQ(int(ymd.year()), 2025);
  EXPECT_EQ(unsigned(ymd.month()), 8);
  EXPECT_EQ(unsigned(ymd.day()), 14);

  tp = StringToTimeISO8601UTC("2025-08-14 12");
  auto sys_days = floor<days>(tp);
  ymd = year_month_day(floor<days>(tp));
  EXPECT_EQ(int(ymd.year()), 2025);
  EXPECT_EQ(unsigned(ymd.month()), 8);
  EXPECT_EQ(unsigned(ymd.day()), 14);
  auto dur = tp - sys_days;
  EXPECT_EQ(dur, std::chrono::hours{12});

  tp = StringToTimeISO8601UTC("2025-08-14 12:34");
  sys_days = floor<days>(tp);
  ymd = year_month_day(floor<days>(tp));
  EXPECT_EQ(int(ymd.year()), 2025);
  EXPECT_EQ(unsigned(ymd.month()), 8);
  EXPECT_EQ(unsigned(ymd.day()), 14);
  dur = tp - sys_days;
  EXPECT_EQ(dur, std::chrono::hours{12} + std::chrono::minutes{34});
}

TEST_F(StringToTimeISO8601UTCTest, ThrowsOnEmptyString) {
  EXPECT_THROW(StringToTimeISO8601UTC(""), std::invalid_argument);
}

TEST_F(StringToTimeISO8601UTCTest, ThrowsOnInvalidMonth) {
  EXPECT_THROW(StringToTimeISO8601UTC("2025-13-01T12:34:56Z"), std::invalid_argument);
}

TEST_F(StringToTimeISO8601UTCTest, ThrowsOnInvalidDay) {
  EXPECT_THROW(StringToTimeISO8601UTC("2025-11-32T12:34:56Z"), std::invalid_argument);
}

TEST_F(StringToTimeISO8601UTCTest, ThrowsOnInvalidHour) {
  EXPECT_THROW(StringToTimeISO8601UTC("2025-11-14T25:34:56Z"), std::invalid_argument);
}

TEST_F(StringToTimeISO8601UTCTest, ThrowsOnInvalidMinute) {
  EXPECT_THROW(StringToTimeISO8601UTC("2025-11-14T12:60:56Z"), std::invalid_argument);
}

TEST_F(StringToTimeISO8601UTCTest, ThrowsOnInvalidSecond) {
  EXPECT_THROW(StringToTimeISO8601UTC("2025-11-14T12:34:61Z"), std::invalid_argument);
}

// ------------------------ Sub-second edge cases ------------------------
TEST_F(StringToTimeISO8601UTCTest, Handles1DigitSubsecond) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56.1Z");
  auto dur = tp - floor<days>(tp);
  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});
  EXPECT_EQ(duration_cast<nanoseconds>(dur).count() % 1000000000, 100'000'000);
}

TEST_F(StringToTimeISO8601UTCTest, Handles2DigitSubsecond) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56.12Z");
  auto dur = tp - floor<days>(tp);
  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});
  EXPECT_EQ(duration_cast<nanoseconds>(dur).count() % 1000000000, 120'000'000);
}

TEST_F(StringToTimeISO8601UTCTest, HandlesZeroZonedTime) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56+00:00");
  auto sys_days = floor<days>(tp);
  auto ymd = year_month_day(floor<days>(tp));
  EXPECT_EQ(int(ymd.year()), 2025);
  EXPECT_EQ(unsigned(ymd.month()), 8);
  EXPECT_EQ(unsigned(ymd.day()), 14);
  auto dur = tp - sys_days;
  EXPECT_EQ(dur, std::chrono::hours{12} + std::chrono::minutes{34} + std::chrono::seconds{56});
}

TEST_F(StringToTimeISO8601UTCTest, HandlesPlusZonedTime) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56+03:00");
  auto sys_days = floor<days>(tp);
  auto ymd = year_month_day(floor<days>(tp));
  EXPECT_EQ(int(ymd.year()), 2025);
  EXPECT_EQ(unsigned(ymd.month()), 8);
  EXPECT_EQ(unsigned(ymd.day()), 14);
  auto dur = tp - sys_days;
  EXPECT_EQ(dur, std::chrono::hours{9} + std::chrono::minutes{34} + std::chrono::seconds{56});
}

TEST_F(StringToTimeISO8601UTCTest, HandlesMinusZonedTime) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56-01:20");
  auto sys_days = floor<days>(tp);
  auto ymd = year_month_day(floor<days>(tp));
  EXPECT_EQ(int(ymd.year()), 2025);
  EXPECT_EQ(unsigned(ymd.month()), 8);
  EXPECT_EQ(unsigned(ymd.day()), 14);
  auto dur = tp - sys_days;
  EXPECT_EQ(dur, std::chrono::hours{13} + std::chrono::minutes{54} + std::chrono::seconds{56});
}

TEST_F(StringToTimeISO8601UTCTest, OldDate) {
  auto tp = StringToTimeISO8601UTC("1996-11-23T03:01:57.12345");
  auto sys_days = floor<days>(tp);
  auto ymd = year_month_day(floor<days>(tp));
  EXPECT_EQ(int(ymd.year()), 1996);
  EXPECT_EQ(unsigned(ymd.month()), 11);
  EXPECT_EQ(unsigned(ymd.day()), 23);
  auto dur = tp - sys_days;
  EXPECT_EQ(dur, std::chrono::hours{3} + std::chrono::minutes{1} + std::chrono::seconds{57} +
                     std::chrono::microseconds{123450});
}

TEST_F(StringToTimeISO8601UTCTest, Handles7DigitSubsecond) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56.12345670Z");
  auto dur = tp - floor<days>(tp);
  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});
  EXPECT_EQ(duration_cast<nanoseconds>(dur).count() % 1000000000, 123456700);
}

TEST_F(StringToTimeISO8601UTCTest, Handles10DigitSubsecond) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56.3508191888Z");
  auto dur = tp - floor<days>(tp);
  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});

  dur -= std::chrono::hours{12} + std::chrono::minutes{34} + std::chrono::seconds{56};

  EXPECT_EQ(duration_cast<nanoseconds>(dur).count() % 10000000000, 350819188);
}

TEST_F(StringToTimeISO8601UTCTest, Handles10DigitSubsecondWithZonedTimePlus) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56.3508191888+01:00");
  auto dur = tp - floor<days>(tp);
  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});

  dur -= std::chrono::hours{11} + std::chrono::minutes{34} + std::chrono::seconds{56};

  EXPECT_EQ(duration_cast<nanoseconds>(dur).count() % 10000000000, 350819188);
}

TEST_F(StringToTimeISO8601UTCTest, Handles10DigitSubsecondWithZonedTimeMinus) {
  auto tp = StringToTimeISO8601UTC("2025-08-14T12:34:56.3508191888-01:30");
  auto dur = tp - floor<days>(tp);
  EXPECT_GT(dur, std::chrono::nanoseconds{0});
  EXPECT_LT(dur, std::chrono::days{1});

  dur -= std::chrono::hours{14} + std::chrono::minutes{4} + std::chrono::seconds{56};

  EXPECT_EQ(duration_cast<nanoseconds>(dur).count() % 10000000000, 350819188);
}

TEST(TimeString, InvalidTimeWindow) {
  EXPECT_THROW(ParseTimeWindow("202"), std::invalid_argument);
  EXPECT_THROW(ParseTimeWindow("2025--26"), std::invalid_argument);
}

TEST(TimeString, ParseTimeWindowTest) {
  using ymd = std::chrono::year_month_day;
  using sys_days = std::chrono::sys_days;

  EXPECT_EQ(
      ParseTimeWindow("2025"),
      std::make_pair(SysTimePoint{sys_days{ymd{std::chrono::year{2025}, std::chrono::January, std::chrono::day{1}}}},
                     SysTimePoint{sys_days{ymd{std::chrono::year{2026}, std::chrono::January, std::chrono::day{1}}}}));
  EXPECT_EQ(ParseTimeWindow("2025-W34"),
            std::make_pair(
                SysTimePoint{sys_days{ymd{std::chrono::year{2025}, std::chrono::month{8}, std::chrono::day{18}}}},
                SysTimePoint{sys_days{ymd{std::chrono::year{2025}, std::chrono::month{8}, std::chrono::day{25}}}}));
  EXPECT_EQ(
      ParseTimeWindow("2025-08"),
      std::make_pair(SysTimePoint{sys_days{ymd{std::chrono::year{2025}, std::chrono::month{8}, std::chrono::day{1}}}},
                     SysTimePoint{sys_days{ymd{std::chrono::year{2025}, std::chrono::month{9}, std::chrono::day{1}}}}));
  EXPECT_EQ(ParseTimeWindow("2025-08-14"),
            std::make_pair(
                SysTimePoint{sys_days{ymd{std::chrono::year{2025}, std::chrono::month{8}, std::chrono::day{14}}}},
                SysTimePoint{sys_days{ymd{std::chrono::year{2025}, std::chrono::month{8}, std::chrono::day{15}}}}));
}

TEST(DateIso8601UTCTest, BasicDate) {
  char buf[16];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14};
  char* end = DateISO8601UTC(tp, buf);
  std::string_view out(buf, static_cast<std::size_t>(end - buf));
  EXPECT_EQ(out, "2025-08-14");
  EXPECT_EQ(end - buf, 10);  // pointer advancement
}

TEST(DateIso8601UTCTest, LeapDay) {
  char buf[16];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2024} / 2 / 29};
  char* end = DateISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2024-02-29");
}

TEST(DateIso8601UTCTest, SingleDigitMonthDayPadding) {
  char buf[16];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 3 / 7};
  char* end = DateISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2025-03-07");  // zero padded
}

TEST(DateIso8601UTCTest, MinimumSupportedEpoch) {
  char buf[16];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{1970} / 1 / 1};
  char* end = DateISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "1970-01-01");
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
  std::string_view sv2(buf, static_cast<std::size_t>(end - buf));
  auto parsed = TryParseTimeRFC7231(sv2);
  EXPECT_NE(parsed, kInvalidTimePoint);
  EXPECT_EQ(time_point_cast<seconds>(parsed), time_point_cast<seconds>(tp));
}

TEST(TimeToStringIso8601UTCFastTest, BasicDateTime) {
  char buf[20];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 14} + std::chrono::hours{12} +
                    std::chrono::minutes{34} + std::chrono::seconds{56};
  char* end = TimeToStringISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2025-08-14T12:34:56Z");
  EXPECT_EQ(end - buf, 20);  // pointer advancement check
}

TEST(TimeToStringIso8601UTCFastTest, Midnight) {
  char buf[20];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2022} / 1 / 1};
  char* end = TimeToStringISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2022-01-01T00:00:00Z");
}

TEST(TimeToStringIso8601UTCFastTest, EndOfYear) {
  char buf[20];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2023} / 12 / 31} + std::chrono::hours{23} +
                    std::chrono::minutes{59} + std::chrono::seconds{59};
  char* end = TimeToStringISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2023-12-31T23:59:59Z");
}

TEST(TimeToStringIso8601UTCFastTest, LeapDay) {
  char buf[20];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2024} / 2 / 29} + std::chrono::hours{6} +
                    std::chrono::minutes{30} + std::chrono::seconds{15};
  char* end = TimeToStringISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2024-02-29T06:30:15Z");
}

TEST(TimeToStringIso8601UTCFastTest, SingleDigitComponentsPadding) {
  char buf[20];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 3 / 7} + std::chrono::hours{1} +
                    std::chrono::minutes{2} + std::chrono::seconds{3};
  char* end = TimeToStringISO8601UTC(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "2025-03-07T01:02:03Z");
}

TEST(TimeToStringIso8601UTCFastTest, RoundTripWithParserNoMillis) {
  char buf[20];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2032} / 5 / 6} + std::chrono::hours{17} +
                    std::chrono::minutes{45} + std::chrono::seconds{12};
  char* end = TimeToStringISO8601UTC(tp, buf);
  std::string_view iso(buf, static_cast<std::size_t>(end - buf));
  SysTimePoint back = StringToTimeISO8601UTC(iso.data(), iso.data() + iso.size());
  char buf2[20];
  char* end2 = TimeToStringISO8601UTC(back, buf2);
  EXPECT_EQ(std::string_view(buf2, static_cast<std::size_t>(end2 - buf2)), iso);
}

// ------------------------ RFC7231 (IMF-fixdate) formatting tests ------------------------

TEST(TimeToStringRFC7231Test, RfcExampleDate) {
  // RFC 7231 example: Sun, 06 Nov 1994 08:49:37 GMT
  char buf[29];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{1994} / 11 / 6} + std::chrono::hours{8} +
                    std::chrono::minutes{49} + std::chrono::seconds{37};
  char* end = TimeToStringRFC7231(tp, buf);
  std::string out(buf, static_cast<std::size_t>(end - buf));
  EXPECT_EQ(out, "Sun, 06 Nov 1994 08:49:37 GMT");
  EXPECT_EQ(out.size(), 29U);  // IMF-fixdate length
}

TEST(TimeToStringRFC7231Test, LeapDay) {
  // 2024-02-29 is a Thursday
  char buf[29];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2024} / 2 / 29} + std::chrono::hours{6} +
                    std::chrono::minutes{30} + std::chrono::seconds{15};
  char* end = TimeToStringRFC7231(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "Thu, 29 Feb 2024 06:30:15 GMT");
}

TEST(TimeToStringRFC7231Test, SingleDigitDayAndMonthPadding) {
  // 2025-03-07 is a Friday, ensure zero padding of day
  char buf[29];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 3 / 7} + std::chrono::hours{1} +
                    std::chrono::minutes{2} + std::chrono::seconds{3};
  char* end = TimeToStringRFC7231(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "Fri, 07 Mar 2025 01:02:03 GMT");
}

TEST(TimeToStringRFC7231Test, MondayWeekdayShiftLogic) {
  // 2025-08-04 is a Monday; tests weekday mapping logic
  char buf[29];
  SysTimePoint tp = std::chrono::sys_days{std::chrono::year{2025} / 8 / 4} + std::chrono::hours{12} +
                    std::chrono::minutes{0} + std::chrono::seconds{0};
  char* end = TimeToStringRFC7231(tp, buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(end - buf)), "Mon, 04 Aug 2025 12:00:00 GMT");
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
  std::vector<size_t> digitPositions = {5, 6, 12, 13, 14, 15, 17, 18, 20, 21, 23, 24};

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
  std::vector<std::string> badTz = {"UTC", "G M", "gmt", "GXT", "XYZ", "GM"};
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

}  // namespace aeronet
