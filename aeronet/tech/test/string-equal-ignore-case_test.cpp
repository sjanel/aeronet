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

TEST(StringEqualIgnoreCase, StringViewVariants) {
  std::string_view lhs = "FooBar";
  std::string_view rhs = "foobar";
  EXPECT_TRUE(CaseInsensitiveEqual(lhs, rhs));
  EXPECT_TRUE(CaseInsensitiveEqual("Foobar", rhs));
  EXPECT_TRUE(CaseInsensitiveEqual(lhs, "fOOBAR"));
  EXPECT_FALSE(CaseInsensitiveEqual(lhs, "foo"));
  EXPECT_FALSE(CaseInsensitiveEqual("foo", "fooo"));
}

TEST(StringEqualIgnoreCase, StartsWith) {
  EXPECT_TRUE(StartsWithCaseInsensitive("HelloWorld", "hello"));
  EXPECT_TRUE(StartsWithCaseInsensitive("HELLO", "hello"));
  EXPECT_FALSE(StartsWithCaseInsensitive("abc", "abcd"));
  EXPECT_FALSE(StartsWithCaseInsensitive("test", "best"));
}

TEST(StringEqualIgnoreCase, HashConsistency) {
  CaseInsensitiveHashFunc hashFunc;
  const std::string_view str1 = "MiXeDCase";
  const std::string_view str2 = "mixedcase";
  EXPECT_EQ(hashFunc(str1), hashFunc(str2));
  EXPECT_NE(hashFunc(str1), hashFunc("different"));
}

TEST(StringEqualIgnoreCase, EqualFuncWrapper) {
  CaseInsensitiveEqualFunc eqFunc;
  EXPECT_TRUE(eqFunc("Sample", "sample"));
  EXPECT_FALSE(eqFunc("Sample", "samples"));
}

}  // namespace aeronet