#include "aeronet/quality-value.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace aeronet::http {

TEST(ParseQualityValue, ValidValues) {
  EXPECT_EQ(ParseQualityValue("1"), kQualityValueMax);
  EXPECT_EQ(ParseQualityValue("1."), kQualityValueMax);
  EXPECT_EQ(ParseQualityValue("1.0"), kQualityValueMax);
  EXPECT_EQ(ParseQualityValue("1.000"), kQualityValueMax);
  EXPECT_EQ(ParseQualityValue("0"), 0);
  EXPECT_EQ(ParseQualityValue("0."), 0);
  EXPECT_EQ(ParseQualityValue("0.5"), 500);
  EXPECT_EQ(ParseQualityValue("0.25"), 250);
  EXPECT_EQ(ParseQualityValue("0.001"), 1);
  EXPECT_EQ(ParseQualityValue("0.999"), 999);
}

TEST(ParseQualityValue, EveryThousandth) {
  std::string buf = "0.000";
  for (int thousandths = 0; thousandths < 1000; ++thousandths) {
    buf[2] = static_cast<char>('0' + (thousandths / 100));
    buf[3] = static_cast<char>('0' + ((thousandths / 10) % 10));
    buf[4] = static_cast<char>('0' + (thousandths % 10));
    EXPECT_EQ(ParseQualityValue(buf), thousandths) << buf;
  }
}

TEST(ParseQualityValue, AboveOneIsZero) {
  for (std::string_view value : {"1.001", "1.5", "1.0001", "2", "42", "10", "inf", "infinity", "1e9", "1e0"}) {
    EXPECT_EQ(ParseQualityValue(value), 0) << value;
  }
}

TEST(ParseQualityValue, MoreThanThreeDecimalsIsZero) {
  for (std::string_view value : {"0.0001", "0.5000", "0.1234", "1.0000"}) {
    EXPECT_EQ(ParseQualityValue(value), 0) << value;
  }
}

TEST(ParseQualityValue, NegativeIsZero) {
  for (std::string_view value : {"-0.5", "-1", "-0", "-inf"}) {
    EXPECT_EQ(ParseQualityValue(value), 0) << value;
  }
}

TEST(ParseQualityValue, InvalidIsZero) {
  for (std::string_view value :
       {"", " ", "abc", "0.5x", "nan", "-nan", "1e400", ".", ".5", "0..5", "+0.5", "0x1", "05", "0,5", "0.5e1"}) {
    EXPECT_EQ(ParseQualityValue(value), 0) << value;
  }
}

// TryParseQualityValue tells an invalid value from 0.
TEST(TryParseQualityValue, InvalidIsTheSentinel) {
  for (std::string_view value : {"", "abc", "1.001", "2", "0.0001", "1.0000", "-0", "+0.5", "inf", "0.5x"}) {
    EXPECT_EQ(TryParseQualityValue(value), kInvalidQualityValue) << value;
  }
  for (std::string_view value : {"0", "0.", "0.000"}) {
    EXPECT_EQ(TryParseQualityValue(value), 0) << value;
  }
  EXPECT_EQ(TryParseQualityValue("0.5;x"), 500);
  EXPECT_EQ(TryParseQualityValue("1.000"), kQualityValueMax);
}

TEST(ParseQualityValue, TrailingPartsIgnored) {
  EXPECT_EQ(ParseQualityValue("0.5 "), 500);
  EXPECT_EQ(ParseQualityValue("0.5\t"), 500);
  EXPECT_EQ(ParseQualityValue("0.5;level=1"), 500);
  EXPECT_EQ(ParseQualityValue("0.5 x"), 500);
  EXPECT_EQ(ParseQualityValue("0.25\tx"), 250);
  EXPECT_EQ(ParseQualityValue("1 "), kQualityValueMax);
  EXPECT_EQ(ParseQualityValue("1;x"), kQualityValueMax);
  EXPECT_EQ(ParseQualityValue("0. "), 0);
}

TEST(ParseQualityValue, LeadingWhitespaceIsInvalid) { EXPECT_EQ(ParseQualityValue(" 0.5"), 0); }

}  // namespace aeronet::http
