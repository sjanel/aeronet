#include "aeronet/http2-config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <utility>

namespace {

using aeronet::Http2Config;

// ============================
// Default Values Tests
// ============================

TEST(Http2Config, DefaultValues) {
  Http2Config config;

  // RFC 9113 SETTINGS defaults
  EXPECT_EQ(config.headerTableSize, 4096U);
  EXPECT_FALSE(config.enablePush);
  EXPECT_EQ(config.maxConcurrentStreams, 100U);
  EXPECT_EQ(config.initialWindowSize, 65535U);
  EXPECT_EQ(config.maxFrameSize, 16384U);
  EXPECT_EQ(config.maxHeaderListSize, 8192U);

  // Connection-level defaults
  EXPECT_EQ(config.connectionWindowSize, 1U << 20);  // 1MB

  // Timeout defaults
  EXPECT_EQ(config.settingsTimeout, std::chrono::milliseconds{5000});
  EXPECT_EQ(config.pingInterval, std::chrono::milliseconds{0});
  EXPECT_EQ(config.pingTimeout, std::chrono::milliseconds{10000});

  // Other defaults
  EXPECT_EQ(config.maxStreamsPerConnection, 0U);
  EXPECT_TRUE(config.enableH2c);
  EXPECT_TRUE(config.enableH2cUpgrade);
  EXPECT_TRUE(config.enablePriority);
  EXPECT_EQ(config.maxPriorityTreeDepth, 256U);
}

// ============================
// Builder Pattern Tests
// ============================

TEST(Http2Config, BuilderPatternSettings) {
  Http2Config config = Http2Config{}
                           .withHeaderTableSize(8192)
                           .withEnablePush(true)
                           .withMergeUnknownRequestHeaders(false)
                           .withMaxConcurrentStreams(50)
                           .withInitialWindowSize(32768)
                           .withMaxFrameSize(32768)
                           .withMaxHeaderListSize(65536);

  EXPECT_EQ(config.headerTableSize, 8192U);
  EXPECT_TRUE(config.enablePush);
  EXPECT_FALSE(config.mergeUnknownRequestHeaders);
  EXPECT_EQ(config.maxConcurrentStreams, 50U);
  EXPECT_EQ(config.initialWindowSize, 32768U);
  EXPECT_EQ(config.maxFrameSize, 32768U);
  EXPECT_EQ(config.maxHeaderListSize, 65536U);
}

TEST(Http2Config, BuilderPatternConnection) {
  Http2Config config = Http2Config{}
                           .withConnectionWindowSize(2 << 20)
                           .withSettingsTimeout(std::chrono::milliseconds{10000})
                           .withPingInterval(std::chrono::milliseconds{30000})
                           .withPingTimeout(std::chrono::milliseconds{5000})
                           .withMaxStreamsPerConnection(1000);

  EXPECT_EQ(config.connectionWindowSize, 2U << 20);
  EXPECT_EQ(config.settingsTimeout, std::chrono::milliseconds{10000});
  EXPECT_EQ(config.pingInterval, std::chrono::milliseconds{30000});
  EXPECT_EQ(config.pingTimeout, std::chrono::milliseconds{5000});
  EXPECT_EQ(config.maxStreamsPerConnection, 1000U);
}

TEST(Http2Config, BuilderPatternFeatures) {
  Http2Config config =
      Http2Config{}.withEnableH2c(false).withEnableH2cUpgrade(false).withEnablePriority(false).withMaxPriorityTreeDepth(
          128);

  EXPECT_FALSE(config.enableH2c);
  EXPECT_FALSE(config.enableH2cUpgrade);
  EXPECT_FALSE(config.enablePriority);
  EXPECT_EQ(config.maxPriorityTreeDepth, 128U);
}

TEST(Http2Config, BuilderChaining) {
  // All builder methods should return reference to same object
  Http2Config config;
  Http2Config& ref1 = config.withHeaderTableSize(1000);
  Http2Config& ref2 = ref1.withMaxConcurrentStreams(50);

  EXPECT_EQ(&config, &ref1);
  EXPECT_EQ(&config, &ref2);
}

// ============================
// Validation Tests - Valid Configs
// ============================

TEST(Http2Config, ValidateDefaultConfig) {
  Http2Config config;
  EXPECT_NO_THROW(config.validate());
}

TEST(Http2Config, ValidateMinMaxFrameSize) {
  Http2Config config = Http2Config{}.withMaxFrameSize(16384);  // RFC minimum
  EXPECT_NO_THROW(config.validate());
}

TEST(Http2Config, ValidateMaxMaxFrameSize) {
  Http2Config config = Http2Config{}.withMaxFrameSize(16777215);  // RFC maximum
  EXPECT_NO_THROW(config.validate());
}

TEST(Http2Config, ValidateMaxWindowSize) {
  Http2Config config = Http2Config{}.withInitialWindowSize(2147483647);  // 2^31 - 1
  EXPECT_NO_THROW(config.validate());
}

TEST(Http2Config, ValidateZeroMaxConcurrentStreams) {
  // Zero means peer cannot open streams - valid per RFC
  Http2Config config = Http2Config{}.withMaxConcurrentStreams(0);
  EXPECT_NO_THROW(config.validate());
}

TEST(Http2Config, ValidateZeroHeaderTableSize) {
  // Zero header table size disables dynamic table - valid per RFC
  Http2Config config = Http2Config{}.withHeaderTableSize(0);
  EXPECT_NO_THROW(config.validate());
}

// ============================
// Validation Tests - Invalid Configs
// ============================

TEST(Http2Config, ValidateMaxFrameSizeTooSmall) {
  Http2Config config = Http2Config{}.withMaxFrameSize(16383);  // Below RFC minimum
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(Http2Config, ValidateMaxFrameSizeTooLarge) {
  Http2Config config = Http2Config{}.withMaxFrameSize(16777216);  // Above RFC maximum
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(Http2Config, ValidateInitialWindowSizeTooLarge) {
  Http2Config config = Http2Config{}.withInitialWindowSize(2147483648U);  // 2^31
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(Http2Config, ValidateConnectionWindowSizeTooLarge) {
  Http2Config config = Http2Config{}.withConnectionWindowSize(2147483648U);  // 2^31
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(Http2Config, ValidateHeaderTableSizeTooLarge) {
  Http2Config config = Http2Config{}.withHeaderTableSize(65537U);  // Above internal limit
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(Http2Config, ValidateZeroMaxHeaderListSize) {
  Http2Config config = Http2Config{}.withMaxHeaderListSize(0);
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(Http2Config, ValidateZeroMaxPriorityTreeDepth) {
  Http2Config config = Http2Config{}.withMaxPriorityTreeDepth(0);
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

// ============================
// Boundary Tests
// ============================

TEST(Http2Config, BoundaryMaxFrameSize) {
  // Test both boundaries
  Http2Config minConfig = Http2Config{}.withMaxFrameSize(16384);
  Http2Config maxConfig = Http2Config{}.withMaxFrameSize(16777215);

  EXPECT_NO_THROW(minConfig.validate());
  EXPECT_NO_THROW(maxConfig.validate());
}

TEST(Http2Config, BoundaryInitialWindowSize) {
  // Test boundary at 2^31-1
  Http2Config maxConfig = Http2Config{}.withInitialWindowSize(2147483647);
  EXPECT_NO_THROW(maxConfig.validate());
}

// ============================
// Copy and Assignment Tests
// ============================

TEST(Http2Config, CopyConstruction) {
  Http2Config original = Http2Config{}.withHeaderTableSize(8192).withMaxConcurrentStreams(200);

  Http2Config copy{original};

  EXPECT_EQ(copy.headerTableSize, 8192U);
  EXPECT_EQ(copy.maxConcurrentStreams, 200U);
}

TEST(Http2Config, CopyAssignment) {
  Http2Config original = Http2Config{}.withHeaderTableSize(8192);
  Http2Config assigned;

  assigned = original;

  EXPECT_EQ(assigned.headerTableSize, 8192U);
}

TEST(Http2Config, MoveConstruction) {
  Http2Config original = Http2Config{}.withHeaderTableSize(8192);
  Http2Config moved{std::move(original)};

  EXPECT_EQ(moved.headerTableSize, 8192U);
}

}  // namespace
