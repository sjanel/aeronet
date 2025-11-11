#include "aeronet/http-server-config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

#include "aeronet/tls-config.hpp"

namespace aeronet {

TEST(HttpServerConfigTest, HeaderKey) {
  HttpServerConfig config;
  config.globalHeaders.clear();
  config.globalHeaders.emplace_back("X-Valid", "value");   // valid
  config.globalHeaders.emplace_back("X-Custom", "value");  // valid
  EXPECT_NO_THROW(config.validate());

  config.globalHeaders.emplace_back("", "value");  // empty key
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.globalHeaders.pop_back();

  config.globalHeaders.emplace_back("Invalid Char!", "value");  // invalid char '!'
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.globalHeaders.pop_back();

  config.globalHeaders.emplace_back("Another@Invalid", "value");  // invalid char '@'
  EXPECT_THROW(config.validate(), std::invalid_argument);
  config.globalHeaders.pop_back();

  config.globalHeaders.emplace_back("X-Valid-Again", "value");  // valid again
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

#if defined(AERONET_ENABLE_OPENSSL) && defined(AERONET_ENABLE_KTLS)
TEST(HttpServerConfigTest, WithTlsKtlsModeEnablesTls) {
  HttpServerConfig config;
  config.withTlsKtlsMode(TLSConfig::KtlsMode::Enabled);
  EXPECT_TRUE(config.tls.enabled);
  EXPECT_EQ(config.tls.ktlsMode, TLSConfig::KtlsMode::Enabled);
}
#endif

}  // namespace aeronet