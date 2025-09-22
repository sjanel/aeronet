#include "time-format.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <string>

#include "timestring.hpp"

namespace aeronet {
using namespace std::chrono;

// Helper to build a deterministic TimePoint
namespace {
TimePointISO8601UTC makeTP(int year, int month, int day, int hour = 0, int min = 0, int sec = 0, int ms = 0) {
  return TimePointISO8601UTC{sys_days{std::chrono::year{year} / month / day} + hours{hour} + minutes{min} +
                             seconds{sec} + milliseconds{ms}};
}
}  // unnamed namespace

TEST(TimeFormatFormatterTest, DefaultIso8601WithMs) {
  // Default formatter path uses Iso8601WithMillis or Iso8601? In formatter, default enum = Iso8601 (no millis)
  auto tp = makeTP(2025, 8, 14, 12, 34, 56, 789);
  // We expect seconds precision for '{}' (no millis)
  EXPECT_EQ(std::vformat(std::string{"{}"}, std::make_format_args(tp)), "2025-08-14T12:34:56Z");
}

TEST(TimeFormatFormatterTest, DateOnlySpecifier) {
  auto tp = makeTP(2024, 2, 29, 6, 7, 8, 9);
  EXPECT_EQ(std::vformat(std::string{"{:d}"}, std::make_format_args(tp)), "2024-02-29");
}

TEST(TimeFormatFormatterTest, Iso8601WithMillisSpecifier) {
  auto tp = makeTP(2031, 12, 5, 23, 59, 1, 7);
  EXPECT_EQ(std::vformat(std::string{"{:ms}"}, std::make_format_args(tp)), "2031-12-05T23:59:01.007Z");
}

TEST(TimeFormatFormatterTest, ZeroPadding) {
  auto tp = makeTP(2001, 3, 7, 1, 2, 3, 4);
  EXPECT_EQ(std::vformat(std::string{"{}"}, std::make_format_args(tp)), "2001-03-07T01:02:03Z");
  EXPECT_EQ(std::vformat(std::string{"{:ms}"}, std::make_format_args(tp)), "2001-03-07T01:02:03.004Z");
}

TEST(TimeFormatFormatterTest, EndOfYear) {
  auto tp = makeTP(1999, 12, 31, 23, 59, 59, 999);
  EXPECT_EQ(std::vformat(std::string{"{}"}, std::make_format_args(tp)), "1999-12-31T23:59:59Z");
  EXPECT_EQ(std::vformat(std::string{"{:ms}"}, std::make_format_args(tp)), "1999-12-31T23:59:59.999Z");
}

TEST(TimeFormatFormatterTest, LeapDay) {
  auto tp = makeTP(2024, 2, 29, 0, 0, 0, 0);
  EXPECT_EQ(std::vformat(std::string{"{}"}, std::make_format_args(tp)), "2024-02-29T00:00:00Z");
  EXPECT_EQ(std::vformat(std::string{"{:d}"}, std::make_format_args(tp)), "2024-02-29");
}

TEST(TimeFormatFormatterTest, RoundTripParseThenFormat) {
  auto tp = makeTP(2033, 5, 6, 7, 8, 9, 123);
  auto isoMs = std::vformat(std::string{"{:ms}"}, std::make_format_args(tp));
  // Parse back with high-precision parser (milliseconds) then format again.
  TimePointISO8601UTC back{StringToTimeISO8601UTC(isoMs)};  // parser should accept .123Z
  EXPECT_EQ(std::vformat(std::string{"{:ms}"}, std::make_format_args(back)), isoMs);
}

TEST(TimeFormatFormatterTest, MultipleFormatsInOneString) {
  auto tp = makeTP(2025, 8, 14, 12, 34, 56, 789);
  std::string multi = "date={:d} base={} ms={:ms}";
  auto multiOut = std::vformat(multi, std::make_format_args(tp, tp, tp));
  EXPECT_EQ(multiOut, "date=2025-08-14 base=2025-08-14T12:34:56Z ms=2025-08-14T12:34:56.789Z");
}

// Invalid specifiers must throw std::format_error when the parser encounters them. We craft runtime strings so the
// compiler doesn't reject them at compile time.
TEST(TimeFormatFormatterTest, InvalidSpecifierSingleChar) {
  auto tp = makeTP(2025, 1, 1);
  std::string fmt = "{:x}";  // 'x' is not supported by our formatter
  EXPECT_THROW((void)std::vformat(fmt, std::make_format_args(tp)), std::format_error);
}

TEST(TimeFormatFormatterTest, InvalidSpecifierPartialMs) {
  auto tp = makeTP(2025, 1, 1);
  std::string fmt = "{:m}";  // partial 'ms'
  EXPECT_THROW((void)std::vformat(fmt, std::make_format_args(tp)), std::format_error);
}

TEST(TimeFormatFormatterTest, InvalidSpecifierExtraChars) {
  auto tp = makeTP(2025, 1, 1);
  std::string fmt1 = "{:msx}";  // extra char after ms
  std::string fmt2 = "{:dx}";   // extra char after d
  EXPECT_THROW((void)std::vformat(fmt1, std::make_format_args(tp)), std::format_error);
  EXPECT_THROW((void)std::vformat(fmt2, std::make_format_args(tp)), std::format_error);
}

TEST(TimeFormatFormatterTest, ChainedFormattingWithOtherTypes) {
  auto tp = makeTP(2025, 8, 14, 12, 34, 56, 0);
  int value = 42;
  std::string fmtChain = "{}|{:d}|{:ms}|{}";
  std::string out = std::vformat(fmtChain, std::make_format_args(tp, tp, tp, value));
  EXPECT_EQ(out, "2025-08-14T12:34:56Z|2025-08-14|2025-08-14T12:34:56.000Z|42");
}

TEST(TimeFormatFormatterTest, MinimumEpoch) {
  TimePointISO8601UTC tp{sys_days{std::chrono::year{1970} / 1 / 1}};
  EXPECT_EQ(std::vformat(std::string{"{}"}, std::make_format_args(tp)), "1970-01-01T00:00:00Z");
  EXPECT_EQ(std::vformat(std::string{"{:ms}"}, std::make_format_args(tp)), "1970-01-01T00:00:00.000Z");
}

}  // namespace aeronet
