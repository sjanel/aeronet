#include "aeronet/timestring.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

using namespace std::chrono;

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

}  // namespace aeronet
