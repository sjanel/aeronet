#include "aeronet/version.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "aeronet/features.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include <brotli/decode.h>

#include <cstdint>
#endif

#ifdef AERONET_ENABLE_PROTOBUF
#include <google/protobuf/stubs/common.h>
#endif

namespace aeronet {

TEST(AeronetVersion, Version) {
  static constexpr auto kVersion = version();

  EXPECT_FALSE(kVersion.empty());
  auto view = fullVersionStringView();
  EXPECT_TRUE(view.contains("aeronet"));
  EXPECT_TRUE(view.contains(kVersion));
  // Multiline format: exactly 8 lines, no trailing newline.
  vector<std::string_view> lines;
  for (std::string_view remaining = view;;) {
    const auto nlPos = remaining.find('\n');
    lines.push_back(remaining.substr(0, nlPos));
    if (nlPos == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(nlPos + 1);
  }
  ASSERT_EQ(lines.size(), 8U);

  EXPECT_TRUE(lines[0].contains(kVersion));
  // Feature lines are indented by two spaces.
  EXPECT_TRUE(lines[1].starts_with("  glaze:"));
  EXPECT_TRUE(lines[2].starts_with("  protobuf:"));
  EXPECT_TRUE(lines[3].starts_with("  tls:"));
  EXPECT_TRUE(lines[4].starts_with("  logging:"));
  EXPECT_TRUE(lines[5].starts_with("  opentelemetry:"));
  EXPECT_TRUE(lines[6].starts_with("  features: "));
  EXPECT_TRUE(lines[7].starts_with("  compression:"));
  // Protobuf line: version of the protobuf headers, and runtime
#ifdef AERONET_ENABLE_PROTOBUF
  std::string expectedProtobufLine =
      "  protobuf: " + google::protobuf::internal::VersionString(GOOGLE_PROTOBUF_VERSION) +
      GOOGLE_PROTOBUF_VERSION_SUFFIX;
#ifdef AERONET_PROTOBUF_LITE
  expectedProtobufLine += " (lite)";
#endif
  EXPECT_EQ(lines[2], expectedProtobufLine);
#else
  EXPECT_EQ(lines[2], "  protobuf: disabled");
#endif
  EXPECT_EQ(lines[2] != "  protobuf: disabled", protobufEnabled());
  // Features line lists each enabled flag, in a deterministic order
  EXPECT_EQ(lines[6].contains("http-server"), httpServerEnabled());
  EXPECT_EQ(lines[6].contains("http-client"), httpClientEnabled());
  EXPECT_EQ(lines[6].contains("http2"), http2Enabled());
  EXPECT_EQ(lines[6].contains("websocket"), webSocketEnabled());
  EXPECT_EQ(lines[6].contains("async-handlers"), asyncHandlersEnabled());
  // Compression line should list enabled codecs separated by comma+space in deterministic order
  // Order enforced in version.hpp join logic: zlib, zstd, brotli (if present)
  if constexpr (zlibEnabled()) {
    EXPECT_TRUE(lines[7].contains("zlib"));
  }
  if constexpr (zstdEnabled()) {
    EXPECT_TRUE(lines[7].contains("zstd"));
  }
  if constexpr (brotliEnabled()) {
    EXPECT_TRUE(lines[7].contains("brotli"));
  }
  // The runtime string should equal the constexpr view content.
  EXPECT_EQ(view, fullVersionStringView());
  // The view should be stable (points to static storage). Multiple calls must return same data pointer.
  EXPECT_EQ(view.data(), fullVersionStringView().data());
}

TEST(AeronetVersion, FullVersionWithRuntime) {
  const std::string base(fullVersionStringView());
  const std::string with_runtime = fullVersionWithRuntime();

#ifdef AERONET_ENABLE_BROTLI
  // When brotli is enabled at compile time, runtime string must append
  // the runtime brotli version in the exact format: " [brotli X.Y.Z]"
  const uint32_t fullVersion = BrotliDecoderVersion();
  const uint32_t major = fullVersion >> 24U;
  const uint32_t minor = (fullVersion >> 12U) & 0xFFFU;
  const uint32_t patch = fullVersion & 0xFFFU;

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

}  // namespace aeronet