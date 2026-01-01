#include "aeronet/bytes-string.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace {
std::string FormatSize(std::uintmax_t size) {
  RawChars out(16);
  AddFormattedSize(size, out);
  return {out.data(), out.size()};
}
}  // namespace

TEST(BytesStringTest, BytesUnder1024) {
  EXPECT_EQ(FormatSize(0), "0 B");
  EXPECT_EQ(FormatSize(1), "1 B");
  EXPECT_EQ(FormatSize(512), "512 B");
  EXPECT_EQ(FormatSize(1023), "1023 B");
}

TEST(BytesStringTest, KiBWithFraction) {
  // 1536 = 1.5 KiB
  EXPECT_EQ(FormatSize(1536), "1.5 KiB");
  // 1024 -> 1.0 KiB (one decimal shown)
  EXPECT_EQ(FormatSize(1024), "1.0 KiB");
  // 10 KiB and above should show integer when >= 10
  EXPECT_EQ(FormatSize(static_cast<std::uintmax_t>(10 * 1024)), "10 KiB");
  // Rounding: 9.96 KiB -> 10 KiB (carry)
  const std::uintmax_t almost10 = (9UL * 1024) + static_cast<std::uintmax_t>(0.96 * 1024);
  EXPECT_EQ(FormatSize(almost10), "10 KiB");
  // Build exact value that rounds to 10.0: frac10 >= 10 case
  const std::uintmax_t ninePointNineSix = (10ULL * 1024ULL) - ((1024ULL * 4ULL) / 100ULL);
  // Ensure we get 10 KiB for a value slightly less than 10*1024 when rounding carries
  EXPECT_EQ(FormatSize(ninePointNineSix), "10 KiB");
}

TEST(BytesStringTest, MiBFormatting) {
  const std::uintmax_t oneMiB = 1024ULL * 1024ULL;
  EXPECT_EQ(FormatSize(oneMiB), "1.0 MiB");
  // 11.78 MiB -> numeric value >= 10 so we print integer rounding: 12 MiB
  EXPECT_EQ(FormatSize(12345678ULL), "12 MiB");

  // 123456789 bytes -> ~117.74 MiB rounds to 118 MiB
  EXPECT_EQ(FormatSize(123456789ULL), "118 MiB");
}

TEST(BytesStringTest, GiBAndTiBFormatting) {
  const std::uintmax_t oneGiB = 1024ULL * 1024ULL * 1024ULL;
  EXPECT_EQ(FormatSize(oneGiB), "1.0 GiB");

  const std::uintmax_t oneTiB = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
  EXPECT_EQ(FormatSize(oneTiB), "1.0 TiB");
}

TEST(BytesStringTest, LargeValues) {
  const std::uintmax_t onePiB = 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
  static constexpr std::uintmax_t kMax = std::numeric_limits<uintmax_t>::max();
  EXPECT_EQ(FormatSize(onePiB), "1.0 PiB");
  EXPECT_EQ(FormatSize(3UL * onePiB), "3.0 PiB");
  EXPECT_EQ(FormatSize(2048UL * onePiB), "2.0 EiB");
  EXPECT_EQ(FormatSize((kMax / 10) - 1), "1.6 EiB");
  EXPECT_EQ(FormatSize(kMax / 10), "1.6 EiB");
  EXPECT_EQ(FormatSize((kMax / 10) + 1), "1.6 EiB");
  EXPECT_EQ(FormatSize(kMax / 2), "8.0 EiB");
  EXPECT_EQ(FormatSize((kMax / 2) + (kMax / 5)), "11 EiB");
  EXPECT_EQ(FormatSize(kMax - 16), "16 EiB");
  EXPECT_EQ(FormatSize(kMax), "16 EiB");
}

}  // namespace aeronet
