#include "aeronet/toupperlower.hpp"

#include <gtest/gtest.h>

#include <list>

namespace aeronet {

template <typename T>
class ToUpperLowerTest : public ::testing::Test {
 public:
  using List = typename std::list<T>;
};

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

}  // namespace aeronet
