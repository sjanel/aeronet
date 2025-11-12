#include "aeronet/stringconv.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace aeronet {

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