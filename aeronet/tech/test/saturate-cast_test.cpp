#include "aeronet/saturate-cast.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>

namespace aeronet {

TEST(SaturateCastTest, ValuesInRangeAreKept) {
  static_assert(SaturateCast<int>(std::size_t{42}) == 42);
  EXPECT_EQ(SaturateCast<int>(std::size_t{0}), 0);
  EXPECT_EQ(SaturateCast<std::int8_t>(-128), std::numeric_limits<std::int8_t>::min());
  EXPECT_EQ(SaturateCast<std::int32_t>(std::int64_t{-12345}), -12345);
  EXPECT_EQ(SaturateCast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()),
            std::uint64_t{std::numeric_limits<std::uint32_t>::max()});
}

TEST(SaturateCastTest, UnsignedToNarrowerSignedIsClampedToMax) {
  // The length of a Winsock send() / recv() call: an int.
  constexpr auto kIntMax = std::numeric_limits<int>::max();
  EXPECT_EQ(SaturateCast<int>(static_cast<std::size_t>(kIntMax)), kIntMax);
  EXPECT_EQ(SaturateCast<int>(static_cast<std::size_t>(kIntMax) + 1U), kIntMax);
  EXPECT_EQ(SaturateCast<int>(std::size_t{std::numeric_limits<std::uint32_t>::max()}), kIntMax);
  EXPECT_EQ(SaturateCast<int>(std::numeric_limits<std::size_t>::max()), kIntMax);
  // Same size, unsigned to signed.
  EXPECT_EQ(SaturateCast<std::int32_t>(std::numeric_limits<std::uint32_t>::max()),
            std::numeric_limits<std::int32_t>::max());
}

TEST(SaturateCastTest, UnsignedToNarrowerUnsignedIsClampedToMax) {
  // The length of a WSABUF or of a ReadFile call: a 32-bit unsigned integer.
  constexpr auto kUint32Max = std::numeric_limits<std::uint32_t>::max();
  EXPECT_EQ(SaturateCast<std::uint32_t>(std::uint64_t{kUint32Max}), kUint32Max);
  EXPECT_EQ(SaturateCast<std::uint32_t>(std::uint64_t{kUint32Max} + 1U), kUint32Max);
  EXPECT_EQ(SaturateCast<std::uint32_t>(std::numeric_limits<std::uint64_t>::max()), kUint32Max);
}

TEST(SaturateCastTest, NegativeValuesAreClampedToMin) {
  EXPECT_EQ(SaturateCast<std::uint32_t>(-1), 0U);
  EXPECT_EQ(SaturateCast<std::uint64_t>(std::numeric_limits<std::int64_t>::min()), 0U);
  EXPECT_EQ(SaturateCast<std::int8_t>(-129), std::numeric_limits<std::int8_t>::min());
  EXPECT_EQ(SaturateCast<std::int32_t>(std::numeric_limits<std::int64_t>::min()),
            std::numeric_limits<std::int32_t>::min());
}

TEST(SaturateCastTest, SignedToNarrowerSignedIsClampedToMax) {
  EXPECT_EQ(SaturateCast<std::int8_t>(128), std::numeric_limits<std::int8_t>::max());
  EXPECT_EQ(SaturateCast<std::int32_t>(std::numeric_limits<std::int64_t>::max()),
            std::numeric_limits<std::int32_t>::max());
}

namespace {

class SaturateCastRandomTest : public ::testing::Test {
 protected:
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937_64 rng{666UL};
  std::uniform_int_distribution<int64_t> s64{std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
  std::uniform_int_distribution<uint64_t> u64{std::numeric_limits<uint64_t>::min(),
                                              std::numeric_limits<uint64_t>::max()};
};

}  // namespace

TEST_F(SaturateCastRandomTest, RandomizedSaturateCastToInt32) {
  for (int i = 0; i < 1000; ++i) {
    const uint64_t uval = u64(rng);
    const int64_t sval = s64(rng);
    EXPECT_EQ(SaturateCast<int32_t>(uval), std::cmp_less_equal(uval, std::numeric_limits<int32_t>::max())
                                               ? static_cast<int32_t>(uval)
                                               : std::numeric_limits<int32_t>::max());
    EXPECT_EQ(SaturateCast<int32_t>(sval),
              static_cast<int32_t>(
                  std::clamp<int64_t>(sval, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max())));
  }
}

}  // namespace aeronet