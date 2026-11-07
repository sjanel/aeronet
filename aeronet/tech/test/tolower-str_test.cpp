#include "aeronet/tolower-str.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <string_view>

#include "aeronet/toupperlower.hpp"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include "aeronet/avx2.hpp"
#endif

namespace aeronet {

TEST(ToLowerStrTest, MixedCase) {
  char str[] = "Hello, World!";
  tolower(str, sizeof(str) - 1);
  EXPECT_STREQ(str, "hello, world!");
}

TEST(ToLowerStrTest, Uppercase) {
  char str[] = "AERONET123";
  tolower(str, sizeof(str) - 1);
  EXPECT_STREQ(str, "aeronet123");
}

TEST(ToLowerStrTest, AlreadyLowercase) {
  char str[] = "already lowercase";
  tolower(str, sizeof(str) - 1);
  EXPECT_STREQ(str, "already lowercase");
}

TEST(ToLowerStrTest, MixedCaseN) {
  const char* src = "Hello, World!";
  char dest[sizeof("Hello, World!")];
  tolower_n(src, sizeof(dest) - 1, dest);
  dest[sizeof(dest) - 1] = '\0';
  EXPECT_STREQ(dest, "hello, world!");
}

TEST(ToLowerStrTest, UppercaseN) {
  const char* src = "AERONET123";
  char dest[sizeof("AERONET123")];
  tolower_n(src, sizeof(dest) - 1, dest);
  dest[sizeof(dest) - 1] = '\0';
  EXPECT_STREQ(dest, "aeronet123");
}

TEST(ToLowerStrTest, AlreadyLowercaseN) {
  const char* src = "already lowercase";
  char dest[sizeof("already lowercase")];
  tolower_n(src, sizeof(dest) - 1, dest);
  dest[sizeof(dest) - 1] = '\0';
  EXPECT_STREQ(dest, "already lowercase");
}

// Every length around the 8-byte steps, for every relative alignment of the buffers, and nothing written around them.
TEST(ToLowerStrTest, ToLowerFromToAllLengthsAndAlignments) {
  static constexpr char kInput[] = "ABCDEFGHijklMNOPqrstUVWX@[`{0123456789-_AZaz";
  static constexpr std::size_t kInputLen = sizeof(kInput) - 1;
  static constexpr char kUntouched = '#';

  for (std::size_t fromOffset = 0; fromOffset < 8; ++fromOffset) {
    for (std::size_t toOffset = 0; toOffset < 8; ++toOffset) {
      for (std::size_t len = 0; len <= kInputLen; ++len) {
        alignas(8) char from[kInputLen + 8];
        alignas(8) char out[kInputLen + 16];
        std::memcpy(from + fromOffset, kInput, len);
        std::memset(out, kUntouched, sizeof(out));

        tolower_n(from + fromOffset, len, out + toOffset);

        for (std::size_t charPos = 0; charPos < sizeof(out); ++charPos) {
          const bool written = charPos >= toOffset && charPos < toOffset + len;
          ASSERT_EQ(out[charPos], written ? tolower(kInput[charPos - toOffset]) : kUntouched)
              << "from offset " << fromOffset << ", to offset " << toOffset << ", length " << len << ", at " << charPos;
        }
      }
    }
  }
}

namespace {
consteval bool ToLowerFromToAtCompileTime() {
  char out[12]{};
  tolower_n("HeLLo WoRLd!", sizeof(out), out);
  return std::string_view(out, sizeof(out)) == "hello world!";
}
static_assert(ToLowerFromToAtCompileTime());
}  // namespace

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
  if (!HasAvx2()) {
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