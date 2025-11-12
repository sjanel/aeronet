#include "aeronet/string-equal-ignore-case.hpp"

#include <gtest/gtest.h>

namespace aeronet {

TEST(StringEqualIgnoreCase, EqualStrings) {
  EXPECT_TRUE(CaseInsensitiveEqual("hello", "HELLO"));
  EXPECT_TRUE(CaseInsensitiveEqual("Hello", "hello"));
  EXPECT_TRUE(CaseInsensitiveEqual("HELLO", "Hello"));
  EXPECT_TRUE(CaseInsensitiveEqual("", ""));
}

TEST(StringEqualIgnoreCase, UnequalStrings) {
  EXPECT_FALSE(CaseInsensitiveEqual("hello", "world"));
  EXPECT_FALSE(CaseInsensitiveEqual("Hello", "world"));
  EXPECT_FALSE(CaseInsensitiveEqual("HELLO", "world"));
  EXPECT_FALSE(CaseInsensitiveEqual("HELLO", "hell"));
}

TEST(StringLessIgnoreCase, LessStrings) {
  EXPECT_FALSE(CaseInsensitiveLess("abc", "ABC"));
  EXPECT_TRUE(CaseInsensitiveLess("abc", "ABcD"));
  EXPECT_FALSE(CaseInsensitiveLess("abc", "AB"));
  EXPECT_FALSE(CaseInsensitiveLess("abcd", "abc"));
}

}  // namespace aeronet