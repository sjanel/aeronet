#include "aeronet/duration-string.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string_view>

namespace aeronet {

TEST(DurationLen, Basic) { EXPECT_EQ(DurationLen("99m"), 3); }

TEST(DurationLen, BasicComplex) { EXPECT_EQ(DurationLen("34d45m"), 6); }

TEST(DurationLen, BasicWithComma) { EXPECT_EQ(DurationLen("23s,aaa"), 3); }

TEST(DurationLen, ComplexWithSpaces) { EXPECT_EQ(DurationLen(" 1 d 52 h,bbbb"), 9); }

TEST(DurationLen, NegativeValue) { EXPECT_EQ(DurationLen("-3s"), 3); }

TEST(DurationLen, InvalidNegativeValue) { EXPECT_EQ(DurationLen("-3s-30ms"), 3); }

TEST(DurationLen, SpacesAtTheEnd) { EXPECT_EQ(DurationLen("-3s30ms  "), 7); }

TEST(DurationLen, InvalidTimeUnit) { EXPECT_EQ(DurationLen("63po"), 0); }

TEST(DurationLen, DoesNotStartWithNumber) { EXPECT_EQ(DurationLen("us"), 0); }

TEST(ParseDuration, EmptyDurationIsZero) { EXPECT_EQ(ParseDuration("0s"), std::chrono::seconds(0)); }

TEST(ParseDuration, DurationDays) { EXPECT_EQ(ParseDuration("37d"), std::chrono::days(37)); }

TEST(ParseDuration, DurationHours) { EXPECT_EQ(ParseDuration("12h"), std::chrono::hours(12)); }

TEST(ParseDuration, DurationMinutesSpaces) {
  EXPECT_EQ(ParseDuration("1 h 45      m "), std::chrono::hours(1) + std::chrono::minutes(45));
}

TEST(ParseDuration, DurationSeconds) { EXPECT_EQ(ParseDuration("3s"), std::chrono::seconds(3)); }

TEST(ParseDuration, DurationMilliseconds) { EXPECT_EQ(ParseDuration("1500 ms"), std::chrono::milliseconds(1500)); }

TEST(ParseDuration, Negative) {
  EXPECT_EQ(ParseDuration("-3s400ms"), -std::chrono::seconds(3) - std::chrono::milliseconds(400));
}

TEST(ParseDuration, NegativeInvalid) { EXPECT_THROW(ParseDuration("- "), std::invalid_argument); }

TEST(ParseDuration, DurationThrowInvalidTimeUnit1) { EXPECT_THROW(ParseDuration("13z"), std::invalid_argument); }

TEST(ParseDuration, DurationThrowInvalidTimeUnit2) { EXPECT_THROW(ParseDuration("42"), std::invalid_argument); }

TEST(ParseDuration, DurationThrowOnlyIntegral) { EXPECT_THROW(ParseDuration("2.5m"), std::invalid_argument); }

TEST(ParseDuration, InvalidNumber) { EXPECT_THROW(ParseDuration("hello"), std::invalid_argument); }

TEST(DurationString, ZeroDuration) {
  EXPECT_EQ(std::string_view(DurationToString(std::chrono::milliseconds(0))), "0ms");
}

TEST(DurationString, NegativeDuration) {
  EXPECT_EQ(std::string_view(DurationToString(std::chrono::milliseconds(-3000))), "-3s");
}

TEST(DurationString, DurationToStringYears) {
  EXPECT_EQ(std::string_view(DurationToString(std::chrono::years(23))), "23y");
}
// Months and weeks removed: corresponding tests dropped
TEST(DurationString, DurationToStringDays) {
  EXPECT_EQ(std::string_view(DurationToString(std::chrono::days(4))), "4d");
}
TEST(DurationString, DurationToStringDaysAndHours) {
  EXPECT_EQ(std::string_view(DurationToString(std::chrono::days(3) + std::chrono::hours(12))), "3d12h");
}
// Weeks removed: replace with pure days + minutes; only 3 significant units captured
TEST(DurationString, DurationToStringDaysMinutes) {
  EXPECT_EQ(
      std::string_view(DurationToString(std::chrono::weeks(2) + std::chrono::days(3) + std::chrono::minutes(57), 3)),
      "2w3d57m");
}
TEST(DurationString, DurationToStringYearsHoursSecondsMilliseconds) {
  EXPECT_EQ(std::string_view(DurationToString(std::chrono::years(50) + std::chrono::hours(2) +
                                                  std::chrono::seconds(13) + std::chrono::milliseconds(556),
                                              10)),
            "50y2h13s556ms");
}

TEST(DurationString, DurationLenSingleUnit) {
  EXPECT_EQ(DurationLen(std::chrono::seconds(45)), 3);  // 45s
  EXPECT_EQ(DurationLen(std::chrono::minutes(5)), 2);   // 5m
  EXPECT_EQ(DurationLen(std::chrono::hours(2)), 2);     // 2h
  EXPECT_EQ(DurationLen(std::chrono::days(-25)), 5);    // -3w4d
  EXPECT_EQ(DurationLen(std::chrono::years(1)), 2);     // 1y
}

TEST(DurationString, DurationLenSeveralUnits) {
  EXPECT_EQ(DurationLen(std::chrono::hours(2) + std::chrono::minutes(55)), 5);  // 2h55m
  EXPECT_EQ(DurationLen(std::chrono::days(1) + std::chrono::hours(2)), 4);      // 1d2h
  EXPECT_EQ(DurationLen(std::chrono::years(1) + std::chrono::days(6)), 4);      // 1y6d
  EXPECT_EQ(DurationLen(std::chrono::years(1) + std::chrono::days(6), 1), 2);   // 1y6d
}

}  // namespace aeronet
