// Unit tests for char-hexadecimal-converter.hpp
#include "aeronet/char-hexadecimal-converter.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <ios>
#include <sstream>
#include <string>

using namespace aeronet;

TEST(CharHexConverter, ToLowerHexBasic) {
  char buf[3] = {};
  char* end = to_lower_hex(',', buf);
  EXPECT_EQ(end, buf + 2);
  buf[2] = '\0';
  EXPECT_STREQ(buf, "2c");

  end = to_lower_hex('?', buf);
  EXPECT_EQ(end, buf + 2);
  buf[2] = '\0';
  EXPECT_STREQ(buf, "3f");
}

TEST(CharHexConverter, ToUpperHexBasic) {
  char buf[3] = {};
  char* end = to_upper_hex(',', buf);
  EXPECT_EQ(end, buf + 2);
  buf[2] = '\0';
  EXPECT_STREQ(buf, "2C");

  end = to_upper_hex('?', buf);
  EXPECT_EQ(end, buf + 2);
  buf[2] = '\0';
  EXPECT_STREQ(buf, "3F");
}

TEST(CharHexConverter, ToHexEdgeValues) {
  char buf[3] = {};
  // 0x00
  to_lower_hex(static_cast<unsigned char>(0x00), buf);
  buf[2] = '\0';
  EXPECT_STREQ(buf, "00");

  // 0x0F
  to_lower_hex(static_cast<unsigned char>(0x0F), buf);
  buf[2] = '\0';
  EXPECT_STREQ(buf, "0f");

  // 0x10
  to_lower_hex(static_cast<unsigned char>(0x10), buf);
  buf[2] = '\0';
  EXPECT_STREQ(buf, "10");

  // 0xFF
  to_lower_hex(static_cast<unsigned char>(0xFF), buf);
  buf[2] = '\0';
  EXPECT_STREQ(buf, "ff");

  // uppercase variant
  to_upper_hex(static_cast<unsigned char>(0xFF), buf);
  buf[2] = '\0';
  EXPECT_STREQ(buf, "FF");
}

TEST(CharHexConverter, FromHexDigitValidDigits) {
  for (char ch = '0'; ch <= '9'; ++ch) {
    int val = from_hex_digit(ch);
    EXPECT_EQ(val, ch - '0') << "char=" << ch;
  }

  for (char ch = 'A'; ch <= 'F'; ++ch) {
    int val = from_hex_digit(ch);
    EXPECT_EQ(val, 10 + (ch - 'A')) << "char=" << ch;
  }

  for (char ch = 'a'; ch <= 'f'; ++ch) {
    int val = from_hex_digit(ch);
    EXPECT_EQ(val, 10 + (ch - 'a')) << "char=" << ch;
  }
}

TEST(CharHexConverter, FromHexDigitInvalid) {
  static constexpr const char invalids[] = {'g', 'G', '/', ':', ' ', 'z', '\0'};
  for (char ch : invalids) {
    int val = from_hex_digit(ch);
    EXPECT_EQ(val, -1) << "char=" << ch;
  }
}

TEST(CharHexConverter, RoundTrip) {
  // Round-trip test: encode then decode two hex digits
  for (int i = 0; i <= 0xFF; ++i) {
    char buf[3] = {};
    to_lower_hex(static_cast<unsigned char>(i), buf);
    // decode high nibble
    int hi = from_hex_digit(buf[0]);
    int lo = from_hex_digit(buf[1]);
    EXPECT_GE(hi, 0);
    EXPECT_GE(lo, 0);
    int value = (hi << 4) | lo;
    EXPECT_EQ(value, i);
  }
}

TEST(CharHexConverterSizeT, HexDigitsAndToLower) {
  EXPECT_EQ(kMaxHexDigitsSizeT, 2UL * sizeof(size_t));

  static constexpr std::size_t vals[]{0U,   1U,    15U,   16U,     255U,
                                      256U, 4095U, 4096U, 0x1234U, static_cast<std::size_t>(-1)};
  for (std::size_t val : vals) {
    std::ostringstream oss;
    oss << std::hex << std::nouppercase << val;
    const std::string expect = oss.str();

    const std::size_t digits = hex_digits(val);
    EXPECT_EQ(digits, expect.size());

    char buf[kMaxHexDigitsSizeT + 1];
    char* end = to_lower_hex(val, buf);
    *end = '\0';
    EXPECT_STREQ(buf, expect.c_str());
    EXPECT_EQ(static_cast<std::size_t>(end - buf), expect.size());
  }
}

TEST(CharHexConverterSizeT, ToUpperHexAndRoundTrip) {
  static constexpr std::size_t vals[]{0U, 1U, 0xAU, 0x10U, 0xFFU, 0x100U, 0xDEADBEEFU};
  for (std::size_t val : vals) {
    char buf[kMaxHexDigitsSizeT + 1];
    char* end = to_upper_hex(val, buf);
    *end = '\0';

    // Reconstruct numeric value from hex digits using from_hex_digit
    std::size_t reconstructed = 0;
    for (char* ptr = buf; ptr < end; ++ptr) {
      int dec = from_hex_digit(*ptr);
      EXPECT_GE(dec, 0);
      reconstructed = (reconstructed << 4) | static_cast<std::size_t>(dec);
    }
    EXPECT_EQ(reconstructed, val);
  }
}

TEST(CharHexConverterSizeT, LargeValueFormatting) {
  const std::size_t maxv = static_cast<std::size_t>(-1);
  char buf[kMaxHexDigitsSizeT + 1];
  char* end = to_lower_hex(maxv, buf);
  *end = '\0';
  // Expect length equals kMaxHexDigitsSizeT
  EXPECT_EQ(static_cast<std::size_t>(end - buf), kMaxHexDigitsSizeT);
}
