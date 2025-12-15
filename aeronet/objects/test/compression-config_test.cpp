#include "aeronet/compression-config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace aeronet {

TEST(CompressionConfigTest, DefaultIsValid) {
  CompressionConfig config;

  EXPECT_NO_THROW(config.validate());
}

TEST(CompressionConfigTest, ZeroEncoderChunkSizeThrows) {
  CompressionConfig config;
  config.encoderChunkSize = 0;

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(CompressionConfigTest, ZlibOK) {
  CompressionConfig config;
  config.zlib.level = 5;

  EXPECT_NO_THROW(config.validate());
}

#ifdef AERONET_ENABLE_ZLIB
TEST(CompressionConfigTest, ZlibInvalidLevelThrows) {
  CompressionConfig config;
  config.zlib.level = 17;

  EXPECT_THROW(config.validate(), std::invalid_argument);

  config.zlib.level = 0;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}
#endif

TEST(CompressionConfigTest, ZstdOK) {
  CompressionConfig config;
  config.zstd.compressionLevel = 3;
  config.zstd.windowLog = 20;

  EXPECT_NO_THROW(config.validate());
}

#ifdef AERONET_ENABLE_ZSTD
TEST(CompressionConfigTest, ZstdInvalidLevelThrows) {
  CompressionConfig config;
  config.zstd.compressionLevel = 29;

  EXPECT_THROW(config.validate(), std::invalid_argument);
}
#endif

TEST(CompressionConfigTest, BrotliOK) {
  CompressionConfig config;
  config.brotli.quality = 6;
  config.brotli.window = 22;

  EXPECT_NO_THROW(config.validate());
}

#ifdef AERONET_ENABLE_BROTLI
TEST(CompressionConfigTest, BrotliInvalidQualityThrows) {
  CompressionConfig config;
  config.brotli.quality = 56;

  EXPECT_THROW(config.validate(), std::invalid_argument);

  config.brotli.quality = -1;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(CompressionConfigTest, BrotliInvalidWindowThrows) {
  CompressionConfig config;
  config.brotli.window = 42;

  EXPECT_THROW(config.validate(), std::invalid_argument);

  config.brotli.window = -4;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}
#endif

}  // namespace aeronet