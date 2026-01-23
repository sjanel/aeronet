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

}  // namespace aeronet