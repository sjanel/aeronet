#include "aeronet/stringconv.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace aeronet {

TEST(StringToIntegral, PositiveValue) {
  EXPECT_EQ(StringToIntegral<uint32_t>("0"), 0);
  EXPECT_EQ(StringToIntegral<uint32_t>("00"), 0);
  EXPECT_EQ(StringToIntegral<uint32_t>("036"), 36);
  EXPECT_EQ(StringToIntegral<uint32_t>("9105470"), 9105470);

  EXPECT_EQ(StringToIntegral<uint32_t>("10YT"), 10);
  EXPECT_EQ(StringToIntegral<int>("11YT"), 11);
  EXPECT_EQ(StringToIntegral<long>("126YT"), 126);
  EXPECT_EQ(StringToIntegral<unsigned char>("37YT"), 37);
  EXPECT_EQ(StringToIntegral<unsigned long>("98YT"), 98);
  EXPECT_THROW(StringToIntegral<uint32_t>("f45"), std::invalid_argument);
  EXPECT_THROW(StringToIntegral<unsigned long>("f45"), std::invalid_argument);
}

TEST(StringToIntegral, NegativeValue) {
  EXPECT_EQ(StringToIntegral<int64_t>("-0"), 0);
  EXPECT_EQ(StringToIntegral<int64_t>("-00"), 0);
  EXPECT_EQ(StringToIntegral<int64_t>("-036"), -36);
  EXPECT_EQ(StringToIntegral<int64_t>("-9105470"), -9105470);
  EXPECT_EQ(StringToIntegral<signed char>("-10YT"), -10);
}

TEST(StringToIntegral, InvalidValue) {
  EXPECT_THROW(StringToIntegral<int32_t>(""), std::invalid_argument);
  EXPECT_THROW(StringToIntegral<int32_t>("--45"), std::invalid_argument);
  EXPECT_THROW(StringToIntegral<int32_t>("+-23"), std::invalid_argument);
  EXPECT_THROW(StringToIntegral<long>("abc"), std::invalid_argument);
}

TEST(StringToIntegral, OutOfRange) {
  EXPECT_THROW(StringToIntegral<int8_t>("128"), std::invalid_argument);
  EXPECT_THROW(StringToIntegral<int8_t>("-129"), std::invalid_argument);
  EXPECT_THROW(StringToIntegral<uint8_t>("-1"), std::invalid_argument);
  EXPECT_THROW(StringToIntegral<uint32_t>("4294967296"), std::invalid_argument);
}

TEST(StringToIntegral, IncorrectBufferLength) {
  const char* str = "12345";
  EXPECT_EQ(StringToIntegral<int32_t>(str, 5), 12345);
  EXPECT_EQ(StringToIntegral<int32_t>(str, 3), 123);
  EXPECT_EQ(StringToIntegral<int32_t>(str, 1), 1);
  EXPECT_EQ(StringToIntegral<int32_t>(str, 2), 12);
}

}  // namespace aeronet