#include "aeronet/internal/is-path-safe.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace aeronet::internal {

namespace {

constexpr std::string_view kUnreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
constexpr std::string_view kPathSubDelimsAndSeparators = "/:@!$&'()*+,;=";

}  // namespace

TEST(IsPathSafe, UnreservedForEveryByte) {
  for (int val = 0; val < 256; ++val) {
    const char ch = static_cast<char>(val);
    EXPECT_EQ(IsUnreserved(ch), kUnreserved.contains(ch)) << val;
  }
}

TEST(IsPathSafe, PathSafeForEveryByte) {
  for (int val = 0; val < 256; ++val) {
    const char ch = static_cast<char>(val);
    EXPECT_EQ(IsPathSafe(ch), kUnreserved.contains(ch) || kPathSubDelimsAndSeparators.contains(ch)) << val;
  }
}

TEST(IsPathSafe, CharsToEncodeInAPath) {
  // Percent sign (a decoded literal is re-encoded), whitespace, line breaks and NUL (header injection), URL
  // delimiters that would change the meaning of a path, and non-ASCII bytes.
  static constexpr char kCharsToEncode[] = "% \t\r\n\0?#[]\"<>\\^`{|}\x7F\x80\xC3\xFF";

  for (char ch : std::string_view(kCharsToEncode, sizeof(kCharsToEncode) - 1U)) {
    EXPECT_FALSE(IsPathSafe(ch)) << static_cast<int>(ch);
  }
}

static_assert(IsPathSafe('a') && IsPathSafe('/') && IsPathSafe('~') && !IsPathSafe(' ') && !IsPathSafe('%'));
static_assert(IsUnreserved('a') && !IsUnreserved('/'));

}  // namespace aeronet::internal
