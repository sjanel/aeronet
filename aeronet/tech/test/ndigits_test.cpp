#include "aeronet/ndigits.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <list>
#include <random>
#include <type_traits>

namespace aeronet {

template <typename T>
class S8NDigitsTest : public ::testing::Test {
 public:
  using List = typename std::list<T>;
};

using S8Types = ::testing::Types<signed char, int8_t>;
TYPED_TEST_SUITE(S8NDigitsTest, S8Types, );

TYPED_TEST(S8NDigitsTest, NDigitsS8) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 1) {
    EXPECT_EQ(ndigits(static_cast<T>(0)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(3)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(78)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(112)), 3U);
    EXPECT_EQ(ndigits(std::numeric_limits<T>::max()), 3U);
    EXPECT_EQ(ndigits(static_cast<T>(-128)), 3U);
    EXPECT_EQ(ndigits(static_cast<T>(-125)), 3U);
    EXPECT_EQ(ndigits(static_cast<T>(-78)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(-10)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(-1)), 1U);

    static_assert(ndigits(std::numeric_limits<T>::max()) == 3U);
    static_assert(ndigits(std::numeric_limits<T>::min()) == 3U);
  }
}

template <typename T>
class S16NDigitsTest : public ::testing::Test {
 public:
  using List = typename std::list<T>;
};

using S16Types = ::testing::Types<short, int16_t>;
TYPED_TEST_SUITE(S16NDigitsTest, S16Types, );

TYPED_TEST(S16NDigitsTest, NDigitsS16) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 2) {
    EXPECT_EQ(ndigits(static_cast<T>(0)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(3)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(78)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(170)), 3U);
    EXPECT_EQ(ndigits(static_cast<T>(9245)), 4U);
    EXPECT_EQ(ndigits(static_cast<T>(31710)), 5U);
    EXPECT_EQ(ndigits(static_cast<T>(-26816)), 5U);
    EXPECT_EQ(ndigits(static_cast<T>(-3686)), 4U);
    EXPECT_EQ(ndigits(static_cast<T>(-686)), 3U);
    EXPECT_EQ(ndigits(static_cast<T>(-10)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(-2)), 1U);

    static_assert(ndigits(std::numeric_limits<T>::max()) == 5U);
    static_assert(ndigits(std::numeric_limits<T>::min()) == 5U);
  }
}

template <typename T>
class S32NDigitsTest : public ::testing::Test {
 public:
  using List = typename std::list<T>;
};

using S32Types = ::testing::Types<int, int32_t>;
TYPED_TEST_SUITE(S32NDigitsTest, S32Types, );

TYPED_TEST(S32NDigitsTest, NDigitsS32) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 4) {
    EXPECT_EQ(ndigits(static_cast<T>(0)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(3)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(78)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(170)), 3U);
    EXPECT_EQ(ndigits(static_cast<T>(9245)), 4U);
    EXPECT_EQ(ndigits(static_cast<T>(35710)), 5U);
    EXPECT_EQ(ndigits(static_cast<T>(100000)), 6U);
    EXPECT_EQ(ndigits(static_cast<T>(1035710)), 7U);
    EXPECT_EQ(ndigits(static_cast<T>(21035710)), 8U);
    EXPECT_EQ(ndigits(static_cast<T>(461035710)), 9U);
    EXPECT_EQ(ndigits(static_cast<T>(1905614858)), 10U);
    EXPECT_EQ(ndigits(static_cast<T>(-1000000000)), 10U);
    EXPECT_EQ(ndigits(static_cast<T>(-908561485)), 9U);
    EXPECT_EQ(ndigits(static_cast<T>(-18561485)), 8U);
    EXPECT_EQ(ndigits(static_cast<T>(-1861485)), 7U);
    EXPECT_EQ(ndigits(static_cast<T>(-186148)), 6U);
    EXPECT_EQ(ndigits(static_cast<T>(-36816)), 5U);
    EXPECT_EQ(ndigits(static_cast<T>(-3686)), 4U);
    EXPECT_EQ(ndigits(static_cast<T>(-686)), 3U);
    EXPECT_EQ(ndigits(static_cast<T>(-10)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(-1)), 1U);

    static_assert(ndigits(std::numeric_limits<T>::max()) == 10U);
    static_assert(ndigits(std::numeric_limits<T>::min()) == 10U);
  }
}

template <typename T>
class S64NDigitsTest : public ::testing::Test {
 public:
  using List = typename std::list<T>;
};

using S64Types = ::testing::Types<long, int64_t>;
TYPED_TEST_SUITE(S64NDigitsTest, S64Types, );

TYPED_TEST(S64NDigitsTest, NDigitsS64) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 8) {
    EXPECT_EQ(ndigits(static_cast<T>(0)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(3)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(78)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(170)), 3U);
    EXPECT_EQ(ndigits(static_cast<T>(9245)), 4U);
    EXPECT_EQ(ndigits(static_cast<T>(35710)), 5U);
    EXPECT_EQ(ndigits(static_cast<T>(100000)), 6U);
    EXPECT_EQ(ndigits(static_cast<T>(1035710)), 7U);
    EXPECT_EQ(ndigits(static_cast<T>(18561485)), 8U);
    EXPECT_EQ(ndigits(static_cast<T>(908561485)), 9U);
    EXPECT_EQ(ndigits(static_cast<T>(5905614858)), 10U);
    EXPECT_EQ(ndigits(static_cast<T>(59085614858)), 11U);
    EXPECT_EQ(ndigits(static_cast<T>(590385614858)), 12U);
    EXPECT_EQ(ndigits(static_cast<T>(2938502856265)), 13U);
    EXPECT_EQ(ndigits(static_cast<T>(29938502856265)), 14U);
    EXPECT_EQ(ndigits(static_cast<T>(299385028562659)), 15U);
    EXPECT_EQ(ndigits(static_cast<T>(7299385028562659)), 16U);
    static_assert(ndigits(static_cast<T>(72993850285626590)) == 17U);
    EXPECT_EQ(ndigits(static_cast<T>(372993850285626590)), 18U);
    EXPECT_EQ(ndigits(static_cast<T>(1000000000000000000)), 19U);
    EXPECT_EQ(ndigits(std::numeric_limits<T>::max()), 19U);
    EXPECT_EQ(ndigits(std::numeric_limits<T>::min()), 19U);
    EXPECT_EQ(ndigits(static_cast<T>(-372909385028562659L)), 18U);
    EXPECT_EQ(ndigits(static_cast<T>(-87299385028566509L)), 17U);
    EXPECT_EQ(ndigits(static_cast<T>(-7299385028562659L)), 16U);
    EXPECT_EQ(ndigits(static_cast<T>(-299385028562659L)), 15U);
    EXPECT_EQ(ndigits(static_cast<T>(-29938502856265L)), 14U);
    EXPECT_EQ(ndigits(static_cast<T>(-2938502856265L)), 13U);
    EXPECT_EQ(ndigits(static_cast<T>(-590385614858L)), 12U);
    EXPECT_EQ(ndigits(static_cast<T>(-59085614858L)), 11U);
    EXPECT_EQ(ndigits(static_cast<T>(-5905614858L)), 10U);
    EXPECT_EQ(ndigits(static_cast<T>(-908561485L)), 9U);
    EXPECT_EQ(ndigits(static_cast<T>(-93058365L)), 8U);
    EXPECT_EQ(ndigits(static_cast<T>(-1861485L)), 7U);
    EXPECT_EQ(ndigits(static_cast<T>(-186148L)), 6U);
    EXPECT_EQ(ndigits(static_cast<T>(-73686L)), 5U);
    EXPECT_EQ(ndigits(static_cast<T>(-3686L)), 4U);
    EXPECT_EQ(ndigits(static_cast<T>(-686L)), 3U);
    EXPECT_EQ(ndigits(static_cast<T>(-10L)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(-7L)), 1U);
  }
}

template <typename T>
class U8NDigitsTest : public ::testing::Test {
 public:
  using List = typename std::list<T>;
};

using U8Types = ::testing::Types<unsigned char, uint8_t>;
TYPED_TEST_SUITE(U8NDigitsTest, U8Types, );

TYPED_TEST(U8NDigitsTest, NDigitsU8) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 1) {
    EXPECT_EQ(ndigits(static_cast<T>(0)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(3)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(78)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(200)), 3U);

    static_assert(ndigits(std::numeric_limits<T>::max()) == 3U);
    static_assert(ndigits(std::numeric_limits<T>::min()) == 1U);
  }
}

template <typename T>
class U16NDigitsTest : public ::testing::Test {
 public:
  using List = typename std::list<T>;
};

using U16Types = ::testing::Types<uint16_t, unsigned short>;
TYPED_TEST_SUITE(U16NDigitsTest, U16Types, );

TYPED_TEST(U16NDigitsTest, NDigitsU16) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 2) {
    EXPECT_EQ(ndigits(static_cast<T>(0)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(10)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(170)), 3U);
    EXPECT_EQ(ndigits(static_cast<T>(4710)), 4U);
    EXPECT_EQ(ndigits(static_cast<T>(46816)), 5U);

    static_assert(ndigits(std::numeric_limits<T>::max()) == 5U);
    static_assert(ndigits(std::numeric_limits<T>::min()) == 1U);
  }
}

template <typename T>
class U32NDigitsTest : public ::testing::Test {
 public:
  using List = typename std::list<T>;
};

using U32Types = ::testing::Types<uint32_t, unsigned int>;
TYPED_TEST_SUITE(U32NDigitsTest, U32Types, );

TYPED_TEST(U32NDigitsTest, NDigitsU32) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 4) {
    EXPECT_EQ(ndigits(static_cast<T>(0U)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(3U)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(78U)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(170U)), 3U);
    EXPECT_EQ(ndigits(static_cast<T>(9245U)), 4U);
    EXPECT_EQ(ndigits(static_cast<T>(35710U)), 5U);
    EXPECT_EQ(ndigits(static_cast<T>(100000U)), 6U);
    EXPECT_EQ(ndigits(static_cast<T>(1035710U)), 7U);
    EXPECT_EQ(ndigits(static_cast<T>(31035710U)), 8U);
    EXPECT_EQ(ndigits(static_cast<T>(561035710U)), 9U);
    EXPECT_EQ(ndigits(static_cast<T>(4105614858U)), 10U);

    static_assert(ndigits(std::numeric_limits<T>::max()) == 10U);
    static_assert(ndigits(std::numeric_limits<T>::min()) == 1U);
  }
}

template <typename T>
class U64NDigitsTest : public ::testing::Test {
 public:
  using List = typename std::list<T>;
};

using U64Types = ::testing::Types<uint64_t, unsigned long>;
TYPED_TEST_SUITE(U64NDigitsTest, U64Types, );

TYPED_TEST(U64NDigitsTest, NDigitsU64) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 8) {
    EXPECT_EQ(ndigits(static_cast<T>(0UL)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(3UL)), 1U);
    EXPECT_EQ(ndigits(static_cast<T>(78UL)), 2U);
    EXPECT_EQ(ndigits(static_cast<T>(170UL)), 3U);
    EXPECT_EQ(ndigits(static_cast<T>(9245UL)), 4U);
    EXPECT_EQ(ndigits(static_cast<T>(35710UL)), 5U);
    EXPECT_EQ(ndigits(static_cast<T>(100000UL)), 6U);
    EXPECT_EQ(ndigits(static_cast<T>(1035710UL)), 7U);
    EXPECT_EQ(ndigits(static_cast<T>(18561485UL)), 8U);
    EXPECT_EQ(ndigits(static_cast<T>(908561485UL)), 9U);
    EXPECT_EQ(ndigits(static_cast<T>(5905614858UL)), 10U);
    EXPECT_EQ(ndigits(static_cast<T>(59085614858UL)), 11U);
    EXPECT_EQ(ndigits(static_cast<T>(590385614858UL)), 12U);
    EXPECT_EQ(ndigits(static_cast<T>(2938502856265UL)), 13U);
    EXPECT_EQ(ndigits(static_cast<T>(29938502856265UL)), 14U);
    EXPECT_EQ(ndigits(static_cast<T>(299385028562659UL)), 15U);
    EXPECT_EQ(ndigits(static_cast<T>(7299385028562659UL)), 16U);
    static_assert(ndigits(static_cast<T>(72993850285626590UL)) == 17U);
    EXPECT_EQ(ndigits(static_cast<T>(372993850285626590UL)), 18U);
    EXPECT_EQ(ndigits(static_cast<T>(8729938502856126509UL)), 19U);
    EXPECT_EQ(ndigits(std::numeric_limits<T>::max()), 20U);
    EXPECT_EQ(ndigits(std::numeric_limits<T>::min()), 1U);
  }
}

namespace {
// Exhaustive threshold tests for powers of 10 boundaries for each type width.
template <typename T>
void ExpectThresholdsUnsigned() {
  using U = T;
  U val = 1;
  for (std::uint8_t digits = 1; digits <= 20U; ++digits) {
    // v == 10^(digits-1)
    EXPECT_EQ(ndigits(static_cast<U>(val)), digits);

    // v-1 has digits-1 (if v>1)
    if (val > 1) {
      EXPECT_EQ(ndigits(static_cast<U>(val - 1)), digits - 1);
    }

    // v*9 yields same digits if it fits in the type
    if (val <= std::numeric_limits<U>::max() / 9) {
      U nine = val * 9;
      EXPECT_EQ(ndigits(static_cast<U>(nine)), digits);
    }

    // advance, careful to stop at overflow
    if (val > std::numeric_limits<U>::max() / 10) {
      break;
    }
    val *= 10;
  }
}

template <typename T>
void ExpectThresholdSigned() {
  using S = T;
  using U = std::make_unsigned_t<S>;

  // positive thresholds
  U val = 1;
  for (std::uint8_t digits = 1; digits <= 20U; ++digits) {
    if (val <= static_cast<U>(std::numeric_limits<S>::max())) {
      EXPECT_EQ(ndigits(static_cast<S>(val)), digits);
      if (val > 1) {
        EXPECT_EQ(ndigits(static_cast<S>(val - 1)), digits - 1);
      }
    }

    // negative thresholds: -v has same digit count as v except sign not counted
    if (val <= static_cast<U>(std::numeric_limits<S>::max())) {
      S neg = static_cast<S>(-static_cast<S>(val));
      EXPECT_EQ(ndigits(neg), digits);
      if (val > 1) {
        EXPECT_EQ(ndigits(static_cast<S>(-(static_cast<S>(val - 1)))), digits - 1);
      }
    }

    if (val > std::numeric_limits<U>::max() / 10) {
      break;
    }
    val *= 10;
  }

  // test min and max explicitly
  EXPECT_EQ(ndigits(std::numeric_limits<S>::max()), ndigits(static_cast<S>(std::numeric_limits<S>::max())));
  EXPECT_EQ(ndigits(std::numeric_limits<S>::min()), ndigits(static_cast<S>(std::numeric_limits<S>::min())));
}

}  // namespace

TYPED_TEST(U16NDigitsTest, Thresholds) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 2) {
    ExpectThresholdsUnsigned<T>();
  }
}

TYPED_TEST(U32NDigitsTest, Thresholds) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 4) {
    ExpectThresholdsUnsigned<T>();
  }
}

TYPED_TEST(U64NDigitsTest, Thresholds) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 8) {
    ExpectThresholdsUnsigned<T>();
  }
}

TYPED_TEST(S8NDigitsTest, Thresholds) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 1) {
    ExpectThresholdSigned<T>();
  }
}

TYPED_TEST(S16NDigitsTest, Thresholds) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 2) {
    ExpectThresholdSigned<T>();
  }
}

TYPED_TEST(S32NDigitsTest, Thresholds) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 4) {
    ExpectThresholdSigned<T>();
  }
}

TYPED_TEST(S64NDigitsTest, Thresholds) {
  using T = TypeParam;
  if constexpr (sizeof(T) == 8) {
    ExpectThresholdSigned<T>();
  }
}

namespace {

int CountDigitsReference(std::signed_integral auto n) {
  using T = decltype(n);
  using U = std::make_unsigned_t<T>;

  // Handle zero explicitly
  if (n == 0) {
    return 1;
  }

  // Convert to unsigned magnitude without invoking undefined behavior for the
  // most-negative value. Use the -(n+1) trick then add one after casting.
  U val;
  if (n < 0) {
    val = static_cast<U>(-(n + 1));
    val += 1U;
  } else {
    val = static_cast<U>(n);
  }

  int count = 0;
  do {
    val /= 10U;
    ++count;
  } while (val != 0U);
  return count;
}

int CountDigitsReference(std::unsigned_integral auto n) {
  int count = 0;
  do {
    n /= 10;
    ++count;
  } while (n != 0);
  return count;
}

}  // namespace

TEST(NDigitsTest, CompareToReferenceImplementationNormalDistribution) {
  std::mt19937_64 rng(20240610);

  // Precompute powers of 10 up to 19 digits (fits in uint64_t)
  static constexpr int kMaxDigits = 19;  // int64_t has up to 19 decimal digits
  uint64_t pow10[kMaxDigits + 1];
  pow10[0] = 1ULL;
  for (int i = 1; i <= kMaxDigits; ++i) {
    pow10[i] = pow10[i - 1] * 10ULL;
  }

  // Choose a normal distribution over digit counts so we don't over-sample very large values.
  std::normal_distribution<double> digit_dist(6.0, 5.0);
  std::bernoulli_distribution sign_dist(0.5);

  static constexpr std::size_t kTests = 1000000;
  for (std::size_t i = 0; i < kTests; ++i) {
    int digits = static_cast<int>(std::llround(digit_dist(rng)));
    digits = std::max(digits, 1);
    digits = std::min(digits, kMaxDigits);

    // For 1-digit values allow zero..9, for others sample in [10^{d-1}, 10^d - 1].
    uint64_t lo = (digits == 1) ? 0ULL : pow10[digits - 1];
    uint64_t hi = pow10[digits] - 1ULL;

    // Clamp hi to int64_t max to avoid overflow when converting to signed.
    const uint64_t kI64Max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    hi = std::min(hi, kI64Max);

    std::uniform_int_distribution<uint64_t> mag_dist(lo, hi);
    uint64_t mag = mag_dist(rng);

    int expected = CountDigitsReference(mag);
    auto actual = ndigits(mag);
    EXPECT_EQ(actual, static_cast<decltype(actual)>(expected)) << "Mismatch for value: " << mag;

    bool negative = sign_dist(rng);
    mag = std::min(mag, kI64Max);
    int64_t val;
    if (negative) {
      // Avoid negating a value that would overflow; cap to int64_t max and negate.
      val = -static_cast<int64_t>(mag);
    } else {
      val = static_cast<int64_t>(mag);
    }

    expected = CountDigitsReference(val);
    actual = ndigits(val);
    EXPECT_EQ(actual, static_cast<decltype(actual)>(expected)) << "Mismatch for value: " << val;
  }
}

}  // namespace aeronet
