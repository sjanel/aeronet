#include "duration-format.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <format>

#include "timedef.hpp"

namespace aeronet::tech {

namespace {
// Helper to format and return std::string
auto FormatDuration(Duration duration) { return std::format("{}", PrettyDuration{duration}); }
}  // namespace

TEST(DurationFormatTest, ZeroDurationPrintsNothing) {
  // Zero should format to an empty string (loop not entered)
  EXPECT_EQ(FormatDuration(Duration::zero()), "");
}

TEST(DurationFormatTest, CompositeFullSpectrum) {
  // 1 year + 2 days + 3 hours + 4 minutes + 5 seconds + 6 milliseconds + 7 microseconds
  Duration compositeDuration = std::chrono::years(1) + std::chrono::days(2) + std::chrono::hours(3) +
                               std::chrono::minutes(4) + std::chrono::seconds(5) + std::chrono::milliseconds(6) +
                               std::chrono::microseconds(7);
  EXPECT_EQ(FormatDuration(compositeDuration), "1y2d3h4m5s6ms7us");
}

TEST(DurationFormatTest, LimitUnits) {
  // 1 year + 2 days + 3 hours + 4 minutes + 5 seconds + 6 milliseconds + 7 microseconds
  Duration compositeDuration = std::chrono::years(1) + std::chrono::days(2) + std::chrono::hours(3) +
                               std::chrono::minutes(4) + std::chrono::seconds(5) + std::chrono::milliseconds(6) +
                               std::chrono::microseconds(7) + std::chrono::nanoseconds(8);

  PrettyDuration dur{compositeDuration};
  EXPECT_EQ(std::format("{:1}", dur), "1y");
  EXPECT_EQ(std::format("{:2}", dur), "1y2d");
  EXPECT_EQ(std::format("{:3}", dur), "1y2d3h");
  EXPECT_EQ(std::format("{:4}", dur), "1y2d3h4m");
  EXPECT_EQ(std::format("{:5}", dur), "1y2d3h4m5s");
  EXPECT_EQ(std::format("{:6}", dur), "1y2d3h4m5s6ms");
  EXPECT_EQ(std::format("{:7}", dur), "1y2d3h4m5s6ms7us");
  EXPECT_EQ(std::format("{:8}", dur), "1y2d3h4m5s6ms7us8ns");
}

TEST(DurationFormatTest, OmitsZeroUnitsMiddle) {
  // 1 day + 5 seconds (no hours / minutes)
  Duration sparseDuration = std::chrono::days(1) + std::chrono::seconds(5);
  EXPECT_EQ(FormatDuration(sparseDuration), "1d5s");
}

TEST(DurationFormatTest, MillisAndMicrosOnly) {
  Duration milliMicroDuration = std::chrono::milliseconds(12) + std::chrono::microseconds(34);
  EXPECT_EQ(FormatDuration(milliMicroDuration), "12ms34us");
}

TEST(DurationFormatTest, MinutesOnly) {
  Duration minutesOnlyDuration = std::chrono::minutes(1);
  EXPECT_EQ(FormatDuration(minutesOnlyDuration), "1m");
}

TEST(DurationFormatTest, MicrosOnly) {
  Duration microsOnlyDuration = std::chrono::microseconds(999);
  EXPECT_EQ(FormatDuration(microsOnlyDuration), "999us");
}

TEST(DurationFormatTest, NanosOnly) {
  Duration nanosOnlyDuration = std::chrono::nanoseconds(750);
  EXPECT_EQ(FormatDuration(nanosOnlyDuration), "750ns");
}

TEST(DurationFormatTest, NegativeDuration) {
  Duration negativeComposite = -(std::chrono::hours(2) + std::chrono::minutes(30) + std::chrono::seconds(1));
  EXPECT_EQ(FormatDuration(negativeComposite), "-2h30m1s");
}

TEST(DurationFormatTest, LargeMixed) {
  Duration largeMixed = std::chrono::years(2) + std::chrono::weeks(6) + std::chrono::minutes(1);
  // std::chrono::years(1) may not exactly equal 365d in all standards, but for arithmetic it's typically 365d.
  // We reconstruct expected by decomposing manually using same algorithm: first years, then days, then ...
  EXPECT_EQ(FormatDuration(largeMixed), "2y42d1m");
}

TEST(DurationFormatTest, OrderingPreference) {
  // Ensure units appear strictly in descending magnitude: y d h m s ms us
  Duration orderingDuration = std::chrono::hours(5) + std::chrono::microseconds(10) + std::chrono::minutes(2) +
                              std::chrono::seconds(3) + std::chrono::milliseconds(4);
  EXPECT_EQ(FormatDuration(orderingDuration), "5h2m3s4ms10us");
}

TEST(DurationFormatTest, MultipleYears) {
  Duration multiYearsDuration = std::chrono::years(3) + std::chrono::days(10);
  EXPECT_EQ(FormatDuration(multiYearsDuration), "3y10d");
}

TEST(DurationFormatTest, NegativeMicroseconds) {
  Duration negativeMicros = -std::chrono::microseconds(15);
  EXPECT_EQ(FormatDuration(negativeMicros), "-15us");
}

TEST(DurationFormatTest, TimePointDiff) {
  TimePoint start = Clock::now();
  TimePoint end = start + std::chrono::microseconds(15);
  EXPECT_EQ(std::format("{}", PrettyDuration(end - start)), "15us");
  EXPECT_EQ(std::format("{:1}", PrettyDuration(end - start)), "15us");
}

TEST(DurationFormatTest, InvalidSpecs) {
  PrettyDuration pd{std::chrono::seconds(1)};
  auto throwTest = [&](const char* fmt) {
    EXPECT_THROW(
        {
          // Force runtime parsing via vformat to bypass compile-time format string validation.
          [[maybe_unused]] auto tmp = std::vformat(fmt, std::make_format_args(pd));
        },
        std::format_error);
  };
  throwTest("{:0}");
  throwTest("{:9}");
  throwTest("{:x}");
  throwTest("{:12}");
  throwTest("{:1x}");
}

}  // namespace aeronet::tech
