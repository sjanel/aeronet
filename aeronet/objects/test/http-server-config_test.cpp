#include "aeronet/http-server-config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

#include "aeronet/http-header.hpp"
#include "aeronet/tls-config.hpp"

namespace aeronet {

TEST(HttpServerConfigTest, HeaderKey1) {
  HttpServerConfig config;
  config.withGlobalHeader(http::Header{"X-Valid", "value"});
  config.withGlobalHeader(http::Header{"X-Custom", "value"});

  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, HeaderKey2) {
  HttpServerConfig config;
  config.withGlobalHeader(http::Header{"", "value"});

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, HeaderKey3) {
  HttpServerConfig config;
  config.withGlobalHeader(http::Header{"Invalid Char!", "value"});  // invalid char '!'
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, HeaderKey4) {
  HttpServerConfig config;
  config.withGlobalHeader(http::Header{"Another@Invalid", "value"});  // invalid char '@'
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, HeaderKey5) {
  HttpServerConfig config;
  config.withGlobalHeader(http::Header{"X-Valid-Again", "value"});  // valid again
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, CompressionConfig) {
  HttpServerConfig config;

#ifdef AERONET_ENABLE_ZLIB
  config.compression.zlib.level = 4;
  EXPECT_NO_THROW(config.validate());

  config.compression.zlib.level = 42;
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.compression.zlib.level = 1;  // reset
#endif

#ifdef AERONET_ENABLE_ZSTD
  config.compression.zstd.compressionLevel = 15;
  EXPECT_NO_THROW(config.validate());

  config.compression.zstd.compressionLevel = 30;
  EXPECT_THROW(config.validate(), std::invalid_argument);
#endif
}

TEST(HttpServerConfigTest, MaxPerEventReadBytesMustMatchChunkSize) {
  HttpServerConfig cfg;
  cfg.withReadChunkStrategy(1024, 4096);  // initial chunk 1kB

  cfg.withMaxPerEventReadBytes(0);
  EXPECT_NO_THROW(cfg.validate());

  cfg.withMaxPerEventReadBytes(1024);
  EXPECT_NO_THROW(cfg.validate());

  cfg.withMaxPerEventReadBytes(512);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

#if defined(AERONET_ENABLE_OPENSSL) && defined(AERONET_ENABLE_KTLS)
TEST(HttpServerConfigTest, WithTlsKtlsModeEnablesTls) {
  HttpServerConfig config;
  config.withTlsKtlsMode(TLSConfig::KtlsMode::Enabled);
  EXPECT_TRUE(config.tls.enabled);
  EXPECT_EQ(config.tls.ktlsMode, TLSConfig::KtlsMode::Enabled);
}
#endif

}  // namespace aeronet