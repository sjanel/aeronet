#include "aeronet/tolower-str.hpp"

#include <gtest/gtest.h>

namespace aeronet {

TEST(ToLowerStrTest, ToLowerInPlace) {
  {
    char str[] = "Hello, World!";
    tolower(str, sizeof(str) - 1);
    EXPECT_STREQ(str, "hello, world!");
  }
  {
    char str[] = "AERONET123";
    tolower(str, sizeof(str) - 1);
    EXPECT_STREQ(str, "aeronet123");
  }
  {
    char str[] = "already lowercase";
    tolower(str, sizeof(str) - 1);
    EXPECT_STREQ(str, "already lowercase");
  }
  {
    char str[] = "MIXED Case StrING 456!";
    tolower(str, sizeof(str) - 1);
    EXPECT_STREQ(str, "mixed case string 456!");
  }
}

TEST(ToLowerStrTest, ToLowerFromTo) {
  {
    const char* src = "Hello, World!";
    char dest[sizeof("Hello, World!")];
    tolower_n(src, sizeof(dest) - 1, dest);
    dest[sizeof(dest) - 1] = '\0';
    EXPECT_STREQ(dest, "hello, world!");
  }
  {
    const char* src = "AERONET123";
    char dest[sizeof("AERONET123")];
    tolower_n(src, sizeof(dest) - 1, dest);
    dest[sizeof(dest) - 1] = '\0';
    EXPECT_STREQ(dest, "aeronet123");
  }
  {
    const char* src = "already lowercase";
    char dest[sizeof("already lowercase")];
    tolower_n(src, sizeof(dest) - 1, dest);
    dest[sizeof(dest) - 1] = '\0';
    EXPECT_STREQ(dest, "already lowercase");
  }
  {
    const char* src = "MIXED Case StrING 456!";
    char dest[sizeof("MIXED Case StrING 456!")];
    tolower_n(src, sizeof(dest) - 1, dest);
    dest[sizeof(dest) - 1] = '\0';
    EXPECT_STREQ(dest, "mixed case string 456!");
  }
}

// Other non ascii bytes should remain unchanged
TEST(ToLowerStrTest, ToLowerNonAscii) {
  {
    char str[] = "Café Noël Ümlaut ñ";
    tolower(str, sizeof(str) - 1);
    EXPECT_STREQ(str, "café noël Ümlaut ñ");
  }
  {
    const char* src = "Café Noël Ümlaut ñ";
    char dest[sizeof("Café Noël Ümlaut ñ")];
    tolower_n(src, sizeof(dest) - 1, dest);
    dest[sizeof(dest) - 1] = '\0';
    EXPECT_STREQ(dest, "café noël Ümlaut ñ");
  }
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
TEST(ToLowerStrTest, ToLowerUsesAvx2ThirtyTwoBytePath) {
  if (!HasAvx2ForToLower()) {
    GTEST_SKIP() << "AVX2 not available on this CPU";
  }

  static constexpr char kInput[] = "ABCDEFGHijklMNOP1234QRSTuvwxYZ!@";
  static constexpr std::size_t kLen = sizeof(kInput) - 1;
  static constexpr char kExpected[] = "abcdefghijklmnop1234qrstuvwxyz!@";
  static_assert(kLen == 32);

  // With AVX2 available, aligned 32-byte input takes the AsciiLowerMask4 path exactly once.
  alignas(8) char out[] = "ABCDEFGHijklMNOP1234QRSTuvwxYZ!@";
  tolower(out, kLen);

  EXPECT_STREQ(out, kExpected);
}
#endif

}  // namespace aeronet