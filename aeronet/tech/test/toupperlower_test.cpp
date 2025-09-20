#include "toupperlower.hpp"

#include <gtest/gtest.h>

namespace aeronet {

TEST(ToUpperLowerTest, ToUpperTest) {
  EXPECT_EQ(toupper('h'), 'H');
  EXPECT_EQ(toupper('e'), 'E');
  EXPECT_EQ(toupper('l'), 'L');
  EXPECT_EQ(toupper('o'), 'O');
  EXPECT_EQ(toupper(' '), ' ');

  EXPECT_EQ(toupper('O'), 'O');
  EXPECT_EQ(toupper('2'), '2');
}

TEST(ToUpperLowerTest, ToLowerTest) {
  EXPECT_EQ(tolower('H'), 'h');
  EXPECT_EQ(tolower('E'), 'e');
  EXPECT_EQ(tolower('L'), 'l');
  EXPECT_EQ(tolower('O'), 'o');
  EXPECT_EQ(tolower(' '), ' ');

  EXPECT_EQ(tolower('o'), 'o');
  EXPECT_EQ(tolower('2'), '2');
}

}  // namespace aeronet
