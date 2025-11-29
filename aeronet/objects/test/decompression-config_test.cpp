#include "aeronet/decompression-config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace aeronet {

TEST(DecompressionConfigTest, ValidDefault) {
  DecompressionConfig cfg;

  EXPECT_NO_THROW(cfg.validate());  // default should not throw

  cfg.enable = true;

  EXPECT_NO_THROW(cfg.validate());  // default should not throw
}

TEST(DecompressionConfigTest, InvalidDecoderChunkSize) {
  DecompressionConfig cfg;

  cfg.enable = true;
  cfg.decoderChunkSize = 0;

  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(DecompressionConfigTest, InvalidDecoderChunkSizeShouldNotThrowIfDisabled) {
  DecompressionConfig cfg;

  cfg.enable = false;
  cfg.decoderChunkSize = 0;

  EXPECT_NO_THROW(cfg.validate());
}

TEST(DecompressionConfigTest, InvalidMaxDecompressedBytes) {
  DecompressionConfig cfg;

  cfg.enable = true;
  cfg.decoderChunkSize = 1024;
  cfg.maxDecompressedBytes = 512;

  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(DecompressionConfigTest, InvalidMaxExpansionRatio) {
  DecompressionConfig cfg;

  cfg.enable = true;
  cfg.maxExpansionRatio = -1.0;

  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(DecompressionConfigTest, InvalidMaxCompressedBytes) {
  DecompressionConfig cfg;

  cfg.enable = true;
  cfg.maxCompressedBytes = 256UL * 1024UL * 1024UL * 1024UL * 1024UL;  // 256 TB

  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

}  // namespace aeronet