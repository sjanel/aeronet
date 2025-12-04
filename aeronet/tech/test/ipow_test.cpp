#include "aeronet/ipow.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>

namespace aeronet {

TEST(MathHelpers, Power10) {
  EXPECT_EQ(ipow10(0U), 1ULL);
  EXPECT_EQ(ipow10(1U), 10ULL);
  EXPECT_EQ(ipow10(2U), 100ULL);
  EXPECT_EQ(ipow10(3U), 1000ULL);
  EXPECT_EQ(ipow10(4U), 10000ULL);
  EXPECT_EQ(ipow10(5U), 100000ULL);
  EXPECT_EQ(ipow10(6U), 1000000ULL);
  EXPECT_EQ(ipow10(7U), 10000000ULL);
  EXPECT_EQ(ipow10(8U), 100000000ULL);
  EXPECT_EQ(ipow10(9U), 1000000000ULL);
  EXPECT_EQ(ipow10(10U), 10000000000ULL);
  EXPECT_EQ(ipow10(11U), 100000000000ULL);
  EXPECT_EQ(ipow10(12U), 1000000000000ULL);
  EXPECT_EQ(ipow10(13U), 10000000000000ULL);
  EXPECT_EQ(ipow10(14U), 100000000000000ULL);
  EXPECT_EQ(ipow10(15U), 1000000000000000ULL);
  EXPECT_EQ(ipow10(16U), 10000000000000000ULL);
  EXPECT_EQ(ipow10(17U), 100000000000000000ULL);
  EXPECT_EQ(ipow10(18U), 1000000000000000000ULL);
  EXPECT_EQ(ipow10(19U), 10000000000000000000ULL);

  static_assert(ipow10(3U) == 1000ULL);
}

class FuzzIpowRng {
 public:
  explicit FuzzIpowRng(uint64_t seed) : _gen(seed) {}

  uint32_t exp() { return static_cast<uint32_t>(_expDist(_gen)); }

 private:
  std::mt19937_64 _gen;
  std::uniform_int_distribution<uint32_t> _expDist{0, 20};
};

TEST(MathHelpers, IpowsCalledAtRuntime) {
  constexpr std::size_t kIterations = 1000;

  FuzzIpowRng rng(666);
  for (std::size_t seed = 0; seed < kIterations; ++seed) {
    uint32_t exp = rng.exp();

    const auto actual = ipow10(exp);
    if (exp <= 19) {
      EXPECT_EQ(static_cast<uint64_t>(std::pow(10.0, static_cast<double>(exp))), actual);
    }
  }
}
}  // namespace aeronet
