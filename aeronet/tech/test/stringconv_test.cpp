#include "stringconv.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aeronet {
TEST(ToChar, Zero) {
  std::string str;
  AppendIntegralToString(str, 0);
  EXPECT_EQ(str, "0");
  EXPECT_EQ(IntegralToString(0), "0");
}

TEST(ToChar, PositiveValue) {
  std::string str("I am a string ");
  AppendIntegralToString(str, 42);
  EXPECT_EQ(str, "I am a string 42");
  AppendIntegralToString(str, 9);
  EXPECT_EQ(str, "I am a string 429");
  EXPECT_EQ(IntegralToString(98124), "98124");
}

TEST(ToChar, NegativeValue) {
  std::string str("I will hold some negative value ");
  AppendIntegralToString(str, -293486);
  EXPECT_EQ(str, "I will hold some negative value -293486");
  AppendIntegralToString(str, -9830346445);
  EXPECT_EQ(str, "I will hold some negative value -293486-9830346445");
  EXPECT_EQ(IntegralToString(-123467), "-123467");
}

TEST(ToChar, UnsignedValue) {
  std::string str("I am a string ");
  AppendIntegralToString(str, 738U);
  EXPECT_EQ(str, "I am a string 738");
  AppendIntegralToString(str, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(str, "I am a string 73818446744073709551615");
  EXPECT_EQ(IntegralToString(630195439576U), "630195439576");
}

TEST(IntegralToCharVector, PositiveValueInt8) {
  EXPECT_EQ(std::string_view(IntegralToCharVector(static_cast<int8_t>(3))), "3");
}

TEST(IntegralToCharVector, NegativeValueInt8) {
  EXPECT_EQ(std::string_view(IntegralToCharVector(static_cast<int8_t>(-11))), "-11");
}

TEST(IntegralToCharVector, PositiveValueInt) { EXPECT_EQ(std::string_view(IntegralToCharVector(34)), "34"); }

TEST(IntegralToCharVector, NegativeValueInt16) {
  EXPECT_EQ(std::string_view(IntegralToCharVector(static_cast<int16_t>(-31678))), "-31678");
}

TEST(IntegralToCharVector, PositiveValueUint64) {
  EXPECT_EQ(std::string_view(IntegralToCharVector(std::numeric_limits<uint64_t>::max())), "18446744073709551615");
}

TEST(StringToIntegral, PositiveValue) {
  EXPECT_EQ(StringToIntegral<uint32_t>("0"), 0);
  EXPECT_EQ(StringToIntegral<uint32_t>("00"), 0);
  EXPECT_EQ(StringToIntegral<uint32_t>("036"), 36);
  EXPECT_EQ(StringToIntegral<uint32_t>("9105470"), 9105470);

  EXPECT_EQ(StringToIntegral<uint32_t>("10YT"), 10);
  EXPECT_THROW(StringToIntegral<uint32_t>("f45"), std::invalid_argument);
}

TEST(StringToIntegral, NegativeValue) {
  EXPECT_EQ(StringToIntegral<int64_t>("-0"), 0);
  EXPECT_EQ(StringToIntegral<int64_t>("-00"), 0);
  EXPECT_EQ(StringToIntegral<int64_t>("-036"), -36);
  EXPECT_EQ(StringToIntegral<int64_t>("-9105470"), -9105470);
}

}  // namespace aeronet