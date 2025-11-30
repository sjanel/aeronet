#include "aeronet/string-trim.hpp"

#include <gtest/gtest.h>

namespace aeronet {

TEST(StringTrimTest, TrimsSpacesAndTabs) {
  EXPECT_EQ(TrimOws("  hello  "), std::string_view("hello"));
  EXPECT_EQ(TrimOws("\thello\t"), std::string_view("hello"));
  EXPECT_EQ(TrimOws(" \thello \t"), std::string_view("hello"));
}

TEST(StringTrimTest, PreservesOtherWhitespace) {
  // Newline is not OWS and should not be trimmed by TrimOws
  EXPECT_EQ(TrimOws("\nhello\n"), std::string_view("\nhello\n"));
  EXPECT_EQ(TrimOws(" \nhello\n "), std::string_view("\nhello\n"));
}

TEST(StringTrimTest, EmptyAndAllWhitespace) {
  EXPECT_EQ(TrimOws(""), std::string_view(""));
  EXPECT_EQ(TrimOws("   \t  "), std::string_view(""));
}

TEST(StringTrimTest, NoTrimNeeded) { EXPECT_EQ(TrimOws("hello"), std::string_view("hello")); }

TEST(StringTrimtest, SpacesInMiddleShouldNotBeTrimmed) {
  EXPECT_EQ(TrimOws("  hello world  "), std::string_view("hello world"));
  EXPECT_EQ(TrimOws("  hello \tworld  "), std::string_view("hello \tworld"));
}

}  // namespace aeronet