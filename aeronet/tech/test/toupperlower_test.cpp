#include "aeronet/toupperlower.hpp"

#include <gtest/gtest.h>

#include <list>
#include <random>

namespace aeronet {

namespace {

template <typename T>
class ToUpperLowerTest : public ::testing::Test {
 public:
  using List = std::list<T>;
};

}  // namespace

using MyTypes = ::testing::Types<char, unsigned char, signed char>;
TYPED_TEST_SUITE(ToUpperLowerTest, MyTypes, );

TYPED_TEST(ToUpperLowerTest, ToUpperTest) {
  using T = TypeParam;
  EXPECT_EQ(toupper(static_cast<T>('h')), static_cast<T>('H'));
  EXPECT_EQ(toupper(static_cast<T>('e')), static_cast<T>('E'));
  EXPECT_EQ(toupper(static_cast<T>('l')), static_cast<T>('L'));
  EXPECT_EQ(toupper(static_cast<T>('o')), static_cast<T>('O'));
  EXPECT_EQ(toupper(static_cast<T>(' ')), static_cast<T>(' '));

  EXPECT_EQ(toupper(static_cast<T>('O')), static_cast<T>('O'));
  EXPECT_EQ(toupper(static_cast<T>('2')), static_cast<T>('2'));
}

TYPED_TEST(ToUpperLowerTest, ToLowerTest) {
  using T = TypeParam;
  EXPECT_EQ(tolower(static_cast<T>('E')), static_cast<T>('e'));
  EXPECT_EQ(tolower(static_cast<T>('L')), static_cast<T>('l'));
  EXPECT_EQ(tolower(static_cast<T>('O')), static_cast<T>('o'));
  EXPECT_EQ(tolower(static_cast<T>(' ')), static_cast<T>(' '));

  EXPECT_EQ(tolower(static_cast<T>('o')), static_cast<T>('o'));
  EXPECT_EQ(tolower(static_cast<T>('2')), static_cast<T>('2'));
}

TYPED_TEST(ToUpperLowerTest, Random) {
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(20251115);
  std::uniform_int_distribution<int> charDist(0, 127);

  for (int i = 0; i < 1000; ++i) {
    const char ch = static_cast<char>(charDist(rng));
    if (ch >= 'a' && ch <= 'z') {
      EXPECT_EQ(tolower(ch), ch);
      EXPECT_EQ(toupper(ch) - 'A', ch - 'a');
    } else if (ch >= 'A' && ch <= 'Z') {
      EXPECT_EQ(tolower(ch) - 'a', ch - 'A');
      EXPECT_EQ(toupper(ch), ch);
    } else {
      EXPECT_EQ(tolower(ch), ch);
      EXPECT_EQ(toupper(ch), ch);
    }
  }
}

}  // namespace aeronet
