#include "aeronet/decimal-writer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>

#include "aeronet/ndigits.hpp"

namespace aeronet {

namespace {

// NOLINTNEXTLINE(bugprone-random-generator-seed,bugprone-throwing-static-initialization)
std::mt19937_64 rng(865719);  // deterministic seed for reproducibility

char buf[std::numeric_limits<uintmax_t>::digits10 + 2];
char refBuf[std::numeric_limits<uintmax_t>::digits10 + 2];

// Powers of ten from 10^0 to 10^19 (10^19 still fits in a uint64_t, max being ~1.8447e19).
// Used to probe digit-count transition boundaries, a classic source of off-by-one bugs
// in hand-written integer-to-string routines.
constexpr std::array<uint64_t, 20> kPow10 = {
    1ULL,
    10ULL,
    100ULL,
    1'000ULL,
    10'000ULL,
    100'000ULL,
    1'000'000ULL,
    10'000'000ULL,
    100'000'000ULL,
    1'000'000'000ULL,
    10'000'000'000ULL,
    100'000'000'000ULL,
    1'000'000'000'000ULL,
    10'000'000'000'000ULL,
    100'000'000'000'000ULL,
    1'000'000'000'000'000ULL,
    10'000'000'000'000'000ULL,
    100'000'000'000'000'000ULL,
    1'000'000'000'000'000'000ULL,
    10'000'000'000'000'000'000ULL,
};

// Compares [buf, end) against std::to_chars(value) for the same value.
template <typename Value>
void CheckResult(char* end, Value value) {
  std::string_view sv(buf, end);

  auto refRes = std::to_chars(refBuf, refBuf + sizeof(refBuf), value);
  std::string_view refSv(refBuf, refRes.ptr);

  EXPECT_EQ(sv, refSv) << "value=" << value;
}

void CheckU16(uint16_t value) { CheckResult(WriteU16(buf, value, ndigits(value)), value); }
void CheckU32(uint32_t value) { CheckResult(WriteU32(buf, value, ndigits(value)), value); }
void CheckU64(uint64_t value) { CheckResult(WriteU64(buf, value, ndigits(value)), value); }
void CheckS64(int64_t value) { CheckResult(WriteSInt(buf, value, ndigits(value)), value); }

}  // namespace

TEST(DecimalWriter, WriteU16) {
  for (uint32_t val = 0; val <= std::numeric_limits<uint16_t>::max(); ++val) {
    CheckU16(static_cast<uint16_t>(val));
  }
}

TEST(DecimalWriter, WriteU32Random) {
  std::uniform_int_distribution<uint32_t> nbDigitsDist(0, std::numeric_limits<uint32_t>::digits10 + 1);
  std::uniform_int_distribution<uint32_t> digitDist(0, 9);

  for (uint32_t idx = 0; idx < 100000; ++idx) {
    const auto nbDigits = nbDigitsDist(rng);
    uint32_t value = 0;
    for (uint32_t i = 0; i < nbDigits; ++i) {
      value = (value * 10) + digitDist(rng);  // unsigned overflow is well-defined (wraps), not UB
    }
    CheckU32(value);
  }
}

TEST(DecimalWriter, WriteU32Boundaries) {
  CheckU32(0);
  CheckU32(1);
  CheckU32(std::numeric_limits<uint32_t>::max());
  CheckU32(std::numeric_limits<uint32_t>::max() - 1);

  for (uint64_t po : kPow10) {
    if (po > std::numeric_limits<uint32_t>::max()) {
      break;
    }
    const auto pv = static_cast<uint32_t>(po);
    CheckU32(pv);
    if (pv > 0) {
      CheckU32(pv - 1);
    }
    if (pv < std::numeric_limits<uint32_t>::max()) {
      CheckU32(pv + 1);
    }
  }
}

TEST(DecimalWriter, WriteU64Random) {
  std::uniform_int_distribution<uint32_t> nbDigitsDist(0, std::numeric_limits<uint64_t>::digits10 + 1);
  std::uniform_int_distribution<uint32_t> digitDist(0, 9);

  for (uint32_t idx = 0; idx < 100000; ++idx) {
    const auto nbDigits = nbDigitsDist(rng);
    uint64_t value = 0;
    for (uint32_t i = 0; i < nbDigits; ++i) {
      value = (value * 10) + digitDist(rng);  // unsigned overflow is well-defined (wraps), not UB
    }
    CheckU64(value);
  }
}

TEST(DecimalWriter, WriteU64Boundaries) {
  CheckU64(0);
  CheckU64(1);
  CheckU64(std::numeric_limits<uint64_t>::max());
  CheckU64(std::numeric_limits<uint64_t>::max() - 1);

  for (uint64_t po : kPow10) {
    CheckU64(po);
    if (po > 0) {
      CheckU64(po - 1);
    }
    if (po < std::numeric_limits<uint64_t>::max()) {
      CheckU64(po + 1);
    }
  }
}

TEST(DecimalWriter, WriteS64Random) {
  std::uniform_int_distribution<uint32_t> nbDigitsDist(0, std::numeric_limits<int64_t>::digits10 + 1);
  std::uniform_int_distribution<uint32_t> digitDist(0, 9);
  std::uniform_int_distribution<uint32_t> signDist(0, 1);

  for (uint32_t idx = 0; idx < 100000; ++idx) {
    const auto nbDigits = nbDigitsDist(rng);

    // Build the magnitude in an *unsigned* accumulator: overflow there is well-defined
    // (wraps modulo 2^64), unlike overflow on a signed int64_t which is UB.
    uint64_t magnitude = 0;
    for (uint32_t i = 0; i < nbDigits; ++i) {
      magnitude = (magnitude * 10) + digitDist(rng);
    }

    const bool negative = signDist(rng) == 0;

    // Apply the sign by computing the two's-complement bit pattern by hand, in unsigned
    // arithmetic (still no UB, even for what will become INT64_MIN), then reinterpret
    // those bits as an int64_t. This correctly covers every representable value,
    // including INT64_MIN, without ever triggering signed overflow.
    const uint64_t bits = negative ? (~magnitude + 1U) : magnitude;
    const int64_t value = std::bit_cast<int64_t>(bits);

    CheckS64(value);
  }
}

TEST(DecimalWriter, WriteS64Boundaries) {
  CheckS64(0);
  CheckS64(1);
  CheckS64(-1);
  CheckS64(std::numeric_limits<int64_t>::max());
  CheckS64(std::numeric_limits<int64_t>::max() - 1);
  CheckS64(std::numeric_limits<int64_t>::min());
  CheckS64(std::numeric_limits<int64_t>::min() + 1);

  for (uint64_t po : kPow10) {
    if (po > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      break;
    }
    const auto pv = static_cast<int64_t>(po);
    CheckS64(pv);
    CheckS64(-pv);
    if (pv > 0) {
      CheckS64(pv - 1);
      CheckS64(-(pv - 1));
    }
    if (pv < std::numeric_limits<int64_t>::max()) {
      CheckS64(pv + 1);
      CheckS64(-(pv + 1));
    }
  }
}

}  // namespace aeronet