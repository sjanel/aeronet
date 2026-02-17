#include <gtest/gtest.h>

#include <string>

#include "aeronet/features.hpp"
#include "aeronet/version.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include <brotli/decode.h>

#include <cstdint>
#endif

TEST(AeronetVersion, Version) {
  static constexpr auto kVersion = aeronet::version();

  EXPECT_FALSE(kVersion.empty());
  auto view = aeronet::fullVersionStringView();
  EXPECT_TRUE(view.contains("aeronet"));
  EXPECT_TRUE(view.contains(std::string(kVersion)));
  // Expect multiline format now including glaze and compression sections
  // (exactly 5 lines, no trailing newline)
  auto first_nl = view.find('\n');
  ASSERT_NE(first_nl, std::string::npos);
  auto second_nl = view.find('\n', first_nl + 1);
  ASSERT_NE(second_nl, std::string::npos);
  auto third_nl = view.find('\n', second_nl + 1);
  ASSERT_NE(third_nl, std::string::npos);
  auto fourth_nl = view.find('\n', third_nl + 1);
  ASSERT_NE(fourth_nl, std::string::npos);
  EXPECT_EQ(view.find('\n', fourth_nl + 1), std::string::npos);  // no 6th line
  // Extract lines
  auto line1 = view.substr(0, first_nl);
  auto line2 = view.substr(first_nl + 1, second_nl - first_nl - 1);
  auto line3 = view.substr(second_nl + 1, third_nl - second_nl - 1);
  auto line4 = view.substr(third_nl + 1, fourth_nl - third_nl - 1);
  auto line5 = view.substr(fourth_nl + 1);

  EXPECT_TRUE(line1.contains(kVersion));
  // Feature lines are indented by two spaces.
  EXPECT_TRUE(line2.rfind("  glaze:", 0) == 0);
  EXPECT_TRUE(line3.rfind("  tls:", 0) == 0);
  EXPECT_TRUE(line4.rfind("  logging:", 0) == 0);
  EXPECT_TRUE(line5.rfind("  compression:", 0) == 0);
  // Compression line should list enabled codecs separated by comma+space in deterministic order
  // Order enforced in version.hpp join logic: zlib, zstd, brotli (if present)
  if constexpr (aeronet::zlibEnabled()) {
    EXPECT_TRUE(line5.contains("zlib"));
  }
  if constexpr (aeronet::zstdEnabled()) {
    EXPECT_TRUE(line5.contains("zstd"));
  }
  if constexpr (aeronet::brotliEnabled()) {
    EXPECT_TRUE(line5.contains("brotli"));
  }
  // The runtime string should equal the constexpr view content.
  EXPECT_EQ(view, aeronet::fullVersionStringView());
  // The view should be stable (points to static storage). Multiple calls must return same data pointer.
  EXPECT_EQ(view.data(), aeronet::fullVersionStringView().data());
}

TEST(AeronetVersion, FullVersionWithRuntime) {
  const std::string base(aeronet::fullVersionStringView());
  const std::string with_runtime = aeronet::fullVersionWithRuntime();

#ifdef AERONET_ENABLE_BROTLI
  // When brotli is enabled at compile time, runtime string must append
  // the runtime brotli version in the exact format: " [brotli X.Y.Z]"
  const uint32_t fullVersion = BrotliDecoderVersion();
  const uint32_t major = fullVersion >> 24;
  const uint32_t minor = (fullVersion >> 12) & 0xFFF;
  const uint32_t patch = fullVersion & 0xFFF;

  const std::string brotliWithVersion =
      "brotli " + std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
  EXPECT_TRUE(with_runtime.contains(base));
  EXPECT_TRUE(with_runtime.contains(brotliWithVersion));
#else
  // When brotli isn't compiled in, fullVersionWithRuntime should equal the
  // compile-time string (no runtime suffix).
  EXPECT_EQ(with_runtime, base);
#endif
}