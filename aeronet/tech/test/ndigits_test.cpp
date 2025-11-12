#include "aeronet/ndigits.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace aeronet {

TEST(MathHelpers, NDigitsS8) {
  EXPECT_EQ(ndigits(static_cast<int8_t>(0)), 1);
  EXPECT_EQ(ndigits(static_cast<int8_t>(3)), 1);
  EXPECT_EQ(ndigits(static_cast<int8_t>(78)), 2);
  EXPECT_EQ(ndigits(static_cast<int8_t>(112)), 3);
  EXPECT_EQ(ndigits(static_cast<int8_t>(-125)), 3);
  EXPECT_EQ(ndigits(static_cast<int8_t>(-10)), 2);
  EXPECT_EQ(ndigits(static_cast<int8_t>(-1)), 1);

  static_assert(ndigits(std::numeric_limits<int8_t>::max()) == 3);
  static_assert(ndigits(std::numeric_limits<int8_t>::min()) == 3);
}

TEST(MathHelpers, NDigitsChar) {
  EXPECT_EQ(ndigits(static_cast<char>(0)), 1);
  EXPECT_EQ(ndigits(static_cast<char>(3)), 1);
  EXPECT_EQ(ndigits(static_cast<char>(78)), 2);
  EXPECT_EQ(ndigits(static_cast<char>(112)), 3);

  static_assert(ndigits(std::numeric_limits<char>::max()) == 3);
  static_assert(ndigits(std::numeric_limits<char>::min()) == 3);
}

TEST(MathHelpers, NDigitsS16) {
  EXPECT_EQ(ndigits(static_cast<int16_t>(0)), 1);
  EXPECT_EQ(ndigits(static_cast<int16_t>(3)), 1);
  EXPECT_EQ(ndigits(static_cast<int16_t>(78)), 2);
  EXPECT_EQ(ndigits(static_cast<int16_t>(170)), 3);
  EXPECT_EQ(ndigits(static_cast<int16_t>(9245)), 4);
  EXPECT_EQ(ndigits(static_cast<int16_t>(31710)), 5);
  EXPECT_EQ(ndigits(static_cast<int16_t>(-26816)), 5);
  EXPECT_EQ(ndigits(static_cast<int16_t>(-3686)), 4);
  EXPECT_EQ(ndigits(static_cast<int16_t>(-686)), 3);
  EXPECT_EQ(ndigits(static_cast<int16_t>(-10)), 2);
  EXPECT_EQ(ndigits(static_cast<int16_t>(-2)), 1);

  static_assert(ndigits(std::numeric_limits<int16_t>::max()) == 5);
  static_assert(ndigits(std::numeric_limits<int16_t>::min()) == 5);
}

TEST(MathHelpers, NDigitsS32) {
  EXPECT_EQ(ndigits(0), 1);
  EXPECT_EQ(ndigits(3), 1);
  EXPECT_EQ(ndigits(78), 2);
  EXPECT_EQ(ndigits(170), 3);
  EXPECT_EQ(ndigits(9245), 4);
  EXPECT_EQ(ndigits(35710), 5);
  EXPECT_EQ(ndigits(100000), 6);
  EXPECT_EQ(ndigits(1035710), 7);
  EXPECT_EQ(ndigits(21035710), 8);
  EXPECT_EQ(ndigits(461035710), 9);
  EXPECT_EQ(ndigits(5905614858), 10);
  EXPECT_EQ(ndigits(-3954784858), 10);
  EXPECT_EQ(ndigits(-908561485), 9);
  EXPECT_EQ(ndigits(-18561485), 8);
  EXPECT_EQ(ndigits(-1861485), 7);
  EXPECT_EQ(ndigits(-186148), 6);
  EXPECT_EQ(ndigits(-36816), 5);
  EXPECT_EQ(ndigits(-3686), 4);
  EXPECT_EQ(ndigits(-686), 3);
  EXPECT_EQ(ndigits(-10), 2);
  EXPECT_EQ(ndigits(-1), 1);

  static_assert(ndigits(std::numeric_limits<int32_t>::max()) == 10);
  static_assert(ndigits(std::numeric_limits<int32_t>::min()) == 10);
}

TEST(MathHelpers, NDigitsS64) {
  EXPECT_EQ(ndigits(0L), 1);
  EXPECT_EQ(ndigits(3L), 1);
  EXPECT_EQ(ndigits(78L), 2);
  EXPECT_EQ(ndigits(170L), 3);
  EXPECT_EQ(ndigits(9245L), 4);
  EXPECT_EQ(ndigits(35710L), 5);
  EXPECT_EQ(ndigits(100000L), 6);
  EXPECT_EQ(ndigits(1035710L), 7);
  EXPECT_EQ(ndigits(18561485L), 8);
  EXPECT_EQ(ndigits(908561485L), 9);
  EXPECT_EQ(ndigits(5905614858L), 10);
  EXPECT_EQ(ndigits(59085614858L), 11);
  EXPECT_EQ(ndigits(590385614858L), 12);
  EXPECT_EQ(ndigits(2938502856265L), 13);
  EXPECT_EQ(ndigits(29938502856265L), 14);
  EXPECT_EQ(ndigits(299385028562659L), 15);
  EXPECT_EQ(ndigits(7299385028562659L), 16);
  static_assert(ndigits(72993850285626590L) == 17);
  EXPECT_EQ(ndigits(372993850285626590L), 18);
  EXPECT_EQ(ndigits(8729938502856126509L), 19);
  EXPECT_EQ(ndigits(std::numeric_limits<int64_t>::max()), 19);
  EXPECT_EQ(ndigits(std::numeric_limits<int64_t>::min()), 19);
  EXPECT_EQ(ndigits(-372909385028562659L), 18);
  EXPECT_EQ(ndigits(-87299385028566509L), 17);
  EXPECT_EQ(ndigits(-7299385028562659L), 16);
  EXPECT_EQ(ndigits(-299385028562659L), 15);
  EXPECT_EQ(ndigits(-29938502856265L), 14);
  EXPECT_EQ(ndigits(-2938502856265L), 13);
  EXPECT_EQ(ndigits(-590385614858L), 12);
  EXPECT_EQ(ndigits(-59085614858L), 11);
  EXPECT_EQ(ndigits(-5905614858L), 10);
  EXPECT_EQ(ndigits(-908561485L), 9);
  EXPECT_EQ(ndigits(-93058365L), 8);
  EXPECT_EQ(ndigits(-1861485L), 7);
  EXPECT_EQ(ndigits(-186148L), 6);
  EXPECT_EQ(ndigits(-73686L), 5);
  EXPECT_EQ(ndigits(-3686L), 4);
  EXPECT_EQ(ndigits(-686L), 3);
  EXPECT_EQ(ndigits(-10L), 2);
}

TEST(MathHelpers, NDigitsU8) {
  EXPECT_EQ(ndigits(static_cast<uint8_t>(0)), 1);
  EXPECT_EQ(ndigits(static_cast<uint8_t>(3)), 1);
  EXPECT_EQ(ndigits(static_cast<uint8_t>(78)), 2);
  EXPECT_EQ(ndigits(static_cast<uint8_t>(200)), 3);

  static_assert(ndigits(std::numeric_limits<uint8_t>::max()) == 3);
  static_assert(ndigits(std::numeric_limits<uint8_t>::min()) == 1);
}

TEST(MathHelpers, NDigitsU16) {
  EXPECT_EQ(ndigits(static_cast<uint16_t>(0)), 1);
  EXPECT_EQ(ndigits(static_cast<uint16_t>(10)), 2);
  EXPECT_EQ(ndigits(static_cast<uint16_t>(170)), 3);
  EXPECT_EQ(ndigits(static_cast<uint16_t>(4710)), 4);
  EXPECT_EQ(ndigits(static_cast<uint16_t>(46816)), 5);

  static_assert(ndigits(std::numeric_limits<uint16_t>::max()) == 5);
  static_assert(ndigits(std::numeric_limits<uint16_t>::min()) == 1);
}

TEST(MathHelpers, NDigitsU32) {
  EXPECT_EQ(ndigits(0U), 1);
  EXPECT_EQ(ndigits(3U), 1);
  EXPECT_EQ(ndigits(78U), 2);
  EXPECT_EQ(ndigits(170U), 3);
  EXPECT_EQ(ndigits(9245U), 4);
  EXPECT_EQ(ndigits(35710U), 5);
  EXPECT_EQ(ndigits(100000U), 6);
  EXPECT_EQ(ndigits(1035710U), 7);
  EXPECT_EQ(ndigits(31035710U), 8);
  EXPECT_EQ(ndigits(561035710U), 9);
  EXPECT_EQ(ndigits(4105614858U), 10);

  static_assert(ndigits(std::numeric_limits<uint32_t>::max()) == 10);
  static_assert(ndigits(std::numeric_limits<uint32_t>::min()) == 1);
}

TEST(MathHelpers, NDigitsU64) {
  EXPECT_EQ(ndigits(0UL), 1);
  EXPECT_EQ(ndigits(3UL), 1);
  EXPECT_EQ(ndigits(78UL), 2);
  EXPECT_EQ(ndigits(170UL), 3);
  EXPECT_EQ(ndigits(9245UL), 4);
  EXPECT_EQ(ndigits(35710UL), 5);
  EXPECT_EQ(ndigits(100000UL), 6);
  EXPECT_EQ(ndigits(1035710UL), 7);
  EXPECT_EQ(ndigits(18561485UL), 8);
  EXPECT_EQ(ndigits(908561485UL), 9);
  EXPECT_EQ(ndigits(5905614858UL), 10);
  EXPECT_EQ(ndigits(59085614858UL), 11);
  EXPECT_EQ(ndigits(590385614858UL), 12);
  EXPECT_EQ(ndigits(2938502856265UL), 13);
  EXPECT_EQ(ndigits(29938502856265UL), 14);
  EXPECT_EQ(ndigits(299385028562659UL), 15);
  EXPECT_EQ(ndigits(7299385028562659UL), 16);
  static_assert(ndigits(72993850285626590UL) == 17);
  EXPECT_EQ(ndigits(372993850285626590UL), 18);
  EXPECT_EQ(ndigits(8729938502856126509UL), 19);
  EXPECT_EQ(ndigits(std::numeric_limits<uint64_t>::max()), 20);
  EXPECT_EQ(ndigits(std::numeric_limits<uint64_t>::min()), 1);
}
}  // namespace aeronet
