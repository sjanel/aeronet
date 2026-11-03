#include "aeronet/url-char-class.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

namespace aeronet::url {

namespace {

constexpr std::string_view kUnreservedChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
constexpr std::string_view kSubDelims = "!$&'()*+,;=";
constexpr std::string_view kSchemeChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-.";

}  // namespace

TEST(UrlCharClass, UnreservedForEveryByte) {
  for (int val = 0; val < 256; ++val) {
    const char ch = static_cast<char>(val);
    EXPECT_EQ(IsUnreserved(ch), kUnreservedChars.contains(ch)) << val;
  }
}

TEST(UrlCharClass, RegNameCharForEveryByte) {
  for (int val = 0; val < 256; ++val) {
    const char ch = static_cast<char>(val);
    EXPECT_EQ(IsRegNameChar(ch), kUnreservedChars.contains(ch) || kSubDelims.contains(ch)) << val;
  }
}

TEST(UrlCharClass, PathCharForEveryByte) {
  for (int val = 0; val < 256; ++val) {
    const char ch = static_cast<char>(val);
    const bool isPChar = kUnreservedChars.contains(ch) || kSubDelims.contains(ch) || ch == ':' || ch == '@';
    EXPECT_EQ(IsPathChar(ch), isPChar || ch == '/') << val;
  }
}

TEST(UrlCharClass, CharsToEncodeInAPath) {
  // Percent sign (a decoded literal is re-encoded), whitespace, line breaks and NUL (header injection), URL
  // delimiters that would change the meaning of a path, and non-ASCII bytes.
  static constexpr char kCharsToEncode[] = "% \t\r\n\0?#[]\"<>\\^`{|}\x7F\x80\xC3\xFF";

  for (char ch : std::string_view(kCharsToEncode, sizeof(kCharsToEncode) - 1U)) {
    EXPECT_FALSE(IsPathChar(ch)) << static_cast<int>(ch);
  }
}

TEST(UrlCharClass, SchemeCharForEveryByte) {
  for (int val = 0; val < 256; ++val) {
    const char ch = static_cast<char>(val);
    EXPECT_EQ(IsSchemeChar(ch), kSchemeChars.contains(ch)) << val;
  }
}

TEST(UrlCharClass, ControlCharForEveryByte) {
  for (int val = 0; val < 256; ++val) {
    EXPECT_EQ(IsControlChar(static_cast<char>(val)), val < 0x20 || val == 0x7F) << val;
  }
}

TEST(UrlCharClass, PredicatesOfAlgorithms) {
  constexpr std::string_view kPath = "/api/v1/users/42;version=2/a:b@c";
  EXPECT_TRUE(std::ranges::all_of(kPath, IsPathChar));
  EXPECT_EQ(std::ranges::find_if_not(kPath, IsRegNameChar) - kPath.begin(), 0);
  constexpr std::string_view kSegments = kPath.substr(1);
  EXPECT_EQ(std::ranges::find_if_not(kSegments, IsUnreserved) - kSegments.begin(), 3);
  EXPECT_FALSE(std::ranges::any_of(kPath, IsControlChar));
  EXPECT_TRUE(std::ranges::any_of(std::string_view("/a\nb"), IsControlChar));
}

static_assert(IsPathChar('a') && IsPathChar('/') && IsPathChar('~') && !IsPathChar(' ') && !IsPathChar('%'));
static_assert(IsUnreserved('a') && !IsUnreserved('/') && !IsUnreserved('%'));
static_assert(IsRegNameChar('!') && !IsRegNameChar(':') && !IsRegNameChar('%'));
static_assert(IsSchemeChar('+') && !IsSchemeChar('_') && !IsSchemeChar(':'));
static_assert(IsControlChar('\0') && IsControlChar('\x7F') && !IsControlChar(' ') && !IsControlChar('\x80'));

}  // namespace aeronet::url
