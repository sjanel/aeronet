#include "aeronet/cctype.hpp"

#include <gtest/gtest.h>

namespace aeronet {

// Compile-time checks for constexpr behavior
static_assert(isdigit('0'), "digit compile-time");
static_assert(!isdigit('a'), "digit compile-time false");
static_assert(islower('a'), "islower compile-time");
static_assert(!islower('A'), "islower compile-time false");
static_assert(isspace(' '), "isspace compile-time");

TEST(Cctype, IsDigitBasic) {
  EXPECT_TRUE(isdigit('0'));
  EXPECT_TRUE(isdigit('5'));
  EXPECT_TRUE(isdigit('9'));

  EXPECT_FALSE(isdigit('/'));
  EXPECT_FALSE(isdigit(':'));
  EXPECT_FALSE(isdigit('a'));
}

TEST(Cctype, IsLowerBasic) {
  EXPECT_TRUE(islower('a'));
  EXPECT_TRUE(islower('m'));
  EXPECT_TRUE(islower('z'));

  EXPECT_FALSE(islower('A'));
  EXPECT_FALSE(islower('0'));
  EXPECT_FALSE(islower('-'));
}

TEST(Cctype, IsSpaceBasic) {
  EXPECT_TRUE(isspace(' '));
  EXPECT_TRUE(isspace('\t'));
  EXPECT_TRUE(isspace('\n'));
  EXPECT_TRUE(isspace('\r'));
  EXPECT_TRUE(isspace('\v'));
  EXPECT_TRUE(isspace('\f'));

  EXPECT_FALSE(isspace('a'));
  EXPECT_FALSE(isspace('0'));
  EXPECT_FALSE(isspace('-'));
}

TEST(Cctype, IsAsciiWhitespace) {
  for (const char ch : {' ', '\t', '\n', '\f', '\r'}) {
    EXPECT_TRUE(IsAsciiWhitespace(ch)) << static_cast<int>(ch);
  }
  // The vertical tab is not ASCII whitespace, unlike for isspace.
  EXPECT_FALSE(IsAsciiWhitespace('\v'));
  // '\b' and '\x0E' surround the control characters of isspace.
  for (const char ch : {'\0', '\b', '\x0E', 'a', '0', '-', '\x85', '\xA0'}) {
    EXPECT_FALSE(IsAsciiWhitespace(ch)) << static_cast<int>(ch);
  }
}

TEST(Cctype, EdgeValues) {
  // check just outside ranges
  EXPECT_FALSE(isdigit('0' - 1));
  EXPECT_FALSE(isdigit('9' + 1));

  EXPECT_FALSE(islower('a' - 1));
  EXPECT_FALSE(islower('z' + 1));

  EXPECT_FALSE(isspace('\t' - 1));
  EXPECT_FALSE(isspace('\r' + 1));
}

}  // namespace aeronet