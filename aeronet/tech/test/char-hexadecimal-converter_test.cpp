// Unit tests for char-hexadecimal-converter.hpp
#include "aeronet/char-hexadecimal-converter.hpp"

#include <gtest/gtest.h>

using namespace aeronet;

TEST(CharHexConverter, ToLowerHexBasic) {
  char buf[3] = {};
  char *end = to_lower_hex(',', buf);
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
  char *end = to_upper_hex(',', buf);
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
  for (char c = '0'; c <= '9'; ++c) {
    int val = from_hex_digit(c);
    EXPECT_EQ(val, c - '0') << "char=" << c;
  }

  for (char c = 'A'; c <= 'F'; ++c) {
    int val = from_hex_digit(c);
    EXPECT_EQ(val, 10 + (c - 'A')) << "char=" << c;
  }

  for (char c = 'a'; c <= 'f'; ++c) {
    int val = from_hex_digit(c);
    EXPECT_EQ(val, 10 + (c - 'a')) << "char=" << c;
  }
}

TEST(CharHexConverter, FromHexDigitInvalid) {
  const char invalids[] = {'g', 'G', '/', ':', ' ', 'z', '\0'};
  for (char c : invalids) {
    int val = from_hex_digit(c);
    EXPECT_EQ(val, -1) << "char=" << c;
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
