#include "aeronet/nchars.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <list>

namespace aeronet {

namespace {

template <typename T>
class S8NCharsTest : public ::testing::Test {
 public:
  using List = std::list<T>;
};

}  // namespace

using S8Types = ::testing::Types<signed char, int8_t>;
TYPED_TEST_SUITE(S8NCharsTest, S8Types, );

TYPED_TEST(S8NCharsTest, NDigitsS8) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 1) {
    EXPECT_EQ(nchars(static_cast<T>(0)), 1U);
    EXPECT_EQ(nchars(static_cast<T>(3)), 1U);
    EXPECT_EQ(nchars(static_cast<T>(78)), 2U);
    EXPECT_EQ(nchars(static_cast<T>(112)), 3U);
    EXPECT_EQ(nchars(std::numeric_limits<T>::max()), 3U);
    EXPECT_EQ(nchars(static_cast<T>(-128)), 4U);
    EXPECT_EQ(nchars(static_cast<T>(-125)), 4U);
    EXPECT_EQ(nchars(static_cast<T>(-78)), 3U);
    EXPECT_EQ(nchars(static_cast<T>(-10)), 3U);
    EXPECT_EQ(nchars(static_cast<T>(-1)), 2U);

    static_assert(nchars(std::numeric_limits<T>::max()) == 3U);
    static_assert(nchars(std::numeric_limits<T>::min()) == 4U);
  }
}

namespace {

template <typename T>
class U8NCharsTest : public ::testing::Test {
 public:
  using List = std::list<T>;
};

}  // namespace

using U8Types = ::testing::Types<unsigned char, uint8_t>;
TYPED_TEST_SUITE(U8NCharsTest, U8Types, );

TYPED_TEST(U8NCharsTest, NDigitsU8) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 1) {
    EXPECT_EQ(nchars(static_cast<T>(0)), 1U);
    EXPECT_EQ(nchars(static_cast<T>(3)), 1U);
    EXPECT_EQ(nchars(static_cast<T>(78)), 2U);
    EXPECT_EQ(nchars(static_cast<T>(200)), 3U);

    static_assert(nchars(std::numeric_limits<T>::max()) == 3U);
    static_assert(nchars(std::numeric_limits<T>::min()) == 1U);
  }
}

}  // namespace aeronet
