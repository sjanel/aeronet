#include <gtest/gtest.h>

#include <string>

#include "aeronet/features.hpp"
#include "aeronet/version.hpp"

TEST(AeronetVersion, Version) {
  static constexpr auto kVersion = aeronet::version();

  EXPECT_FALSE(kVersion.empty());
  auto view = aeronet::fullVersionStringView();
  EXPECT_TRUE(view.contains("aeronet"));
  EXPECT_TRUE(view.contains(std::string(kVersion)));
  // Expect multiline format now including compression section (exactly 4 lines, no trailing newline)
  auto first_nl = view.find('\n');
  ASSERT_NE(first_nl, std::string::npos);
  auto second_nl = view.find('\n', first_nl + 1);
  ASSERT_NE(second_nl, std::string::npos);
  auto third_nl = view.find('\n', second_nl + 1);
  ASSERT_NE(third_nl, std::string::npos);
  EXPECT_EQ(view.find('\n', third_nl + 1), std::string::npos);  // no 5th line
  // Extract lines
  auto line1 = view.substr(0, first_nl);
  auto line2 = view.substr(first_nl + 1, second_nl - first_nl - 1);
  auto line3 = view.substr(second_nl + 1, third_nl - second_nl - 1);
  auto line4 = view.substr(third_nl + 1);
  EXPECT_TRUE(line1.contains(kVersion));
  // Feature lines are indented by two spaces.
  EXPECT_TRUE(line2.rfind("  tls:", 0) == 0);
  EXPECT_TRUE(line3.rfind("  logging:", 0) == 0);
  EXPECT_TRUE(line4.rfind("  compression:", 0) == 0);
  // Compression line should list enabled codecs separated by comma+space in deterministic order
  // Order enforced in version.hpp join logic: zlib, zstd, brotli (if present)
  if constexpr (aeronet::zlibEnabled()) {
    EXPECT_TRUE(line4.contains("zlib"));
  }
  if constexpr (aeronet::zstdEnabled()) {
    EXPECT_TRUE(line4.contains("zstd"));
  }
  if constexpr (aeronet::brotliEnabled()) {
    EXPECT_TRUE(line4.contains("brotli"));
  }
  // The runtime string should equal the constexpr view content.
  EXPECT_EQ(view, aeronet::fullVersionStringView());
  // The view should be stable (points to static storage). Multiple calls must return same data pointer.
  EXPECT_EQ(view.data(), aeronet::fullVersionStringView().data());
}