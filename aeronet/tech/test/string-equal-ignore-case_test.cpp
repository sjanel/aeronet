#include "aeronet/string-equal-ignore-case.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>

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

namespace {

std::string LowerCase(std::string_view sv) {
  std::string result;
  result.reserve(sv.size());
  std::ranges::transform(sv, std::back_inserter(result),
                         [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
  return result;
}

// Helper reference implementations using tolower for ASCII only
bool ReferenceCaseInsensitiveEqual(std::string_view lhs, std::string_view rhs) {
  return LowerCase(lhs) == LowerCase(rhs);
}

bool ReferenceCaseInsensitiveLess(std::string_view lhs, std::string_view rhs) {
  return LowerCase(lhs) < LowerCase(rhs);
}

bool ReferenceStartsWithCaseInsensitive(std::string_view str, std::string_view prefix) {
  if (prefix.size() > str.size()) {
    return false;
  }
  return LowerCase(str.substr(0, prefix.size())) == LowerCase(prefix);
}
}  // namespace

TEST(StringEqualIgnoreCase, FuzzRandomAsciiEqual) {
  std::mt19937_64 rng(123456789);
  std::uniform_int_distribution<std::size_t> lenDist(0, 32);
  std::uniform_int_distribution<int> charDist(0x20, 0x7E);  // printable ASCII
  std::string s1;
  std::string s2;
  for (int iteration = 0; iteration < 2000; ++iteration) {
    std::size_t sz = lenDist(rng);
    s1.clear();
    s2.clear();
    for (std::size_t i = 0; i < sz; ++i) {
      char ch = static_cast<char>(charDist(rng));
      // randomly change case for letters
      if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
        if (std::uniform_int_distribution<int>(0, 1)(rng) != 0) {
          ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        } else {
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
      }
      s1.push_back(ch);
      // produce s2 by randomizing case per character
      char ch2 = ch;
      if ((ch2 >= 'A' && ch2 <= 'Z') || (ch2 >= 'a' && ch2 <= 'z')) {
        if (std::uniform_int_distribution<int>(0, 1)(rng) != 0) {
          ch2 = static_cast<char>(std::toupper(static_cast<unsigned char>(ch2)));
        } else {
          ch2 = static_cast<char>(std::tolower(static_cast<unsigned char>(ch2)));
        }
      }
      s2.push_back(ch2);
    }
    EXPECT_EQ(CaseInsensitiveEqual(s1, s2), ReferenceCaseInsensitiveEqual(s1, s2));
    EXPECT_EQ(StartsWithCaseInsensitive(s1, s2), ReferenceStartsWithCaseInsensitive(s1, s2));
    // also cross-compare with different lengths occasionally
    if (iteration % 10 == 0) {
      std::string s3 = s1 + static_cast<char>(charDist(rng));
      EXPECT_EQ(CaseInsensitiveEqual(s1, s3), ReferenceCaseInsensitiveEqual(s1, s3));
    }
  }
}

TEST(StringLessIgnoreCase, FuzzRandomAsciiLess) {
  std::mt19937_64 rng(987654321);
  std::uniform_int_distribution<std::size_t> lenDist(0, 32);
  std::uniform_int_distribution<int> charDist(0x20, 0x7E);  // printable ASCII
  std::string lhs;
  std::string rhs;

  for (int iteration = 0; iteration < 2000; ++iteration) {
    std::size_t na = lenDist(rng);
    std::size_t nb = lenDist(rng);
    lhs.clear();
    rhs.clear();
    for (std::size_t i = 0; i < na; ++i) {
      char ch = static_cast<char>(charDist(rng));
      if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
        if (std::uniform_int_distribution<int>(0, 1)(rng) != 0) {
          ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        } else {
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
      }
      lhs.push_back(ch);
    }
    for (std::size_t i = 0; i < nb; ++i) {
      char ch = static_cast<char>(charDist(rng));
      if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
        if (std::uniform_int_distribution<int>(0, 1)(rng) != 0) {
          ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        } else {
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
      }
      rhs.push_back(ch);
    }
    EXPECT_EQ(CaseInsensitiveLess(lhs, rhs), ReferenceCaseInsensitiveLess(lhs, rhs));
    EXPECT_EQ(StartsWithCaseInsensitive(lhs, rhs), ReferenceStartsWithCaseInsensitive(lhs, rhs));
  }
}

}  // namespace aeronet