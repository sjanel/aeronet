#include "ipow.hpp"

#include <gtest/gtest.h>

namespace aeronet {

TEST(MathHelpers, PowerPositiveBase) {
  EXPECT_EQ(ipow(0, 0), 1);
  EXPECT_EQ(ipow(0, 2), 0);
  EXPECT_EQ(ipow(1, 0), 1);
  EXPECT_EQ(ipow(1, 23), 1);
  EXPECT_EQ(ipow(1, 24), 1);
  EXPECT_EQ(ipow(3, 2), 9);
  EXPECT_EQ(ipow(4, 3), 64);
  EXPECT_EQ(ipow(17, 5), 1419857);
  EXPECT_EQ(ipow(10, 10), 10000000000LL);
  EXPECT_EQ(ipow(0, 0), 1);

  static_assert(ipow(5, 3) == 125);
}

TEST(MathHelpers, PowerNegativeBase) {
  EXPECT_EQ(ipow(-1, 0), 1);
  EXPECT_EQ(ipow(-1, 2), 1);
  EXPECT_EQ(ipow(-1, 3), -1);
  EXPECT_EQ(ipow(-1, 32), 1);
  EXPECT_EQ(ipow(-1, 33), -1);
  EXPECT_EQ(ipow(-2, 1), -2);
  EXPECT_EQ(ipow(-3, 3), -27);
  EXPECT_EQ(ipow(-23, 13), -504036361936467383LL);

  static_assert(ipow(-7, 0) == 1);
}

TEST(MathHelpers, Power10) {
  EXPECT_EQ(ipow10(0), 1);
  EXPECT_EQ(ipow10(1), 10);
  EXPECT_EQ(ipow10(2), 100);
  EXPECT_EQ(ipow10(3), 1000);
  EXPECT_EQ(ipow10(4), 10000);
  EXPECT_EQ(ipow10(5), 100000);
  EXPECT_EQ(ipow10(6), 1000000);
  EXPECT_EQ(ipow10(7), 10000000);
  EXPECT_EQ(ipow10(8), 100000000);
  EXPECT_EQ(ipow10(9), 1000000000);
  EXPECT_EQ(ipow10(10), 10000000000);
  EXPECT_EQ(ipow10(11), 100000000000LL);
  EXPECT_EQ(ipow10(12), 1000000000000LL);
  EXPECT_EQ(ipow10(13), 10000000000000LL);
  EXPECT_EQ(ipow10(14), 100000000000000LL);
  EXPECT_EQ(ipow10(15), 1000000000000000LL);
  EXPECT_EQ(ipow10(16), 10000000000000000LL);
  EXPECT_EQ(ipow10(17), 100000000000000000LL);
  EXPECT_EQ(ipow10(18), 1000000000000000000LL);

  static_assert(ipow10(3) == 1000);
}
}  // namespace aeronet
