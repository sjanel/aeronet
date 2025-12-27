#include "aeronet/http-server-config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "aeronet/decompression-config.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/tls-config.hpp"

namespace aeronet {

TEST(HttpServerConfigTest, WithGlobalHeadersShouldReplaceAllList) {
  HttpServerConfig config;
  config.withGlobalHeaders(
      std::vector<http::Header>{http::Header{"X-Valid", "value"}, http::Header{"X-Custom", "value"}});
  EXPECT_EQ(config.globalHeaders.nbConcatenatedStrings(), 2U);
  config.withGlobalHeaders(
      std::vector<http::Header>{http::Header{"X-Valid2", "value"}, http::Header{"X-Custom2", "value"}});
  EXPECT_EQ(config.globalHeaders.nbConcatenatedStrings(), 2U);
  config.withGlobalHeaders({});
  EXPECT_TRUE(config.globalHeaders.empty());

  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, AddGlobalHeader) {
  HttpServerConfig config;
  config.addGlobalHeader(http::Header{"X-Test", "value"});
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, HeaderKey1) {
  HttpServerConfig config;
  config.withGlobalHeaders(
      std::vector<http::Header>{http::Header{"X-Valid", "value"}, http::Header{"X-Custom", "value"}});

  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, HeaderKey2) {
  HttpServerConfig config;
  config.globalHeaders.append(":value");

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, HeaderKey3) {
  HttpServerConfig config;
  config.globalHeaders.append("Invalid Char!:value");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, HeaderKey4) {
  HttpServerConfig config;
  config.globalHeaders.append("Another@Invalid:value");  // invalid char '@'
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, ReservedGlobalHeaderShouldThrow) {
  HttpServerConfig config;
  config.globalHeaders.append("Content-Length:10");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, InvalidGlobalHeaderShouldThrow1) {
  HttpServerConfig config;
  config.globalHeaders.append("Invalid\nHeader:value");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, InvalidGlobalHeaderShouldThrow2) {
  HttpServerConfig config;
  config.globalHeaders.append("InvalidNoColon");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, InvalidGlobalHeaderShouldThrow3) {
  HttpServerConfig config;
  config.globalHeaders.append("X-Custom: value\x7F");  // DEL control char
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, TooManyGlobalHeaders1) {
  HttpServerConfig config;
  std::vector<http::Header> headers;
  headers.reserve(HttpServerConfig::kMaxGlobalHeaders + 1);
  for (std::size_t i = 0; i < HttpServerConfig::kMaxGlobalHeaders + 1; ++i) {
    headers.emplace_back("X-Header-" + std::to_string(i), "value");
  }
  EXPECT_THROW(config.withGlobalHeaders(headers), std::invalid_argument);
}

TEST(HttpServerConfigTest, TooManyGlobalHeaders2) {
  HttpServerConfig config;
  for (std::size_t i = 0; i < HttpServerConfig::kMaxGlobalHeaders + 1; ++i) {
    config.addGlobalHeader(http::Header{"X-Header-" + std::to_string(i), "value"});
  }
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, InvalidGlobalHeaderValueWithControlChars) {
  HttpServerConfig config;
  config.globalHeaders.append("X-Test:value\x01");  // control char 0x01
  EXPECT_THROW(config.validate(), std::invalid_argument);
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

TEST(HttpServerConfigTest, WithTlsKtlsModeEnablesTls) {
  HttpServerConfig config;
  config.withTlsKtlsMode(TLSConfig::KtlsMode::Enabled);
  EXPECT_TRUE(config.tls.enabled);
  EXPECT_EQ(config.tls.ktlsMode, TLSConfig::KtlsMode::Enabled);
}

TEST(HttpServerConfigTest, InvalidKeepAliveTimeoutThrows) {
  HttpServerConfig config;
  config.withKeepAliveTimeout(std::chrono::milliseconds{-1});
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, InvalidPollIntervalThrows) {
  HttpServerConfig config;
  config.withPollInterval(std::chrono::milliseconds{0});
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, InvalidHeaderReadTimeoutThrows) {
  HttpServerConfig config;
  config.withHeaderReadTimeout(std::chrono::milliseconds{-1});
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, InvalidBodyReadTimeoutThrows) {
  HttpServerConfig config;
  config.withBodyReadTimeout(std::chrono::milliseconds{-1});
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, TooSmallOutboundBufferBytesThrows) {
  HttpServerConfig config;
  config.withMaxOutboundBufferBytes(64);  // less than minimum of 1kB
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, WithoutTlsShouldDisableTls) {
  HttpServerConfig config;
  config.withTlsAlpnProtocols({"http/1.1"});
  EXPECT_TRUE(config.tls.enabled);
  config.withoutTls();
  EXPECT_FALSE(config.tls.enabled);
}

TEST(HttpServerConfigTest, TooLargePollIntervalValueThrows) {
  HttpServerConfig config;
  config.withPollInterval(std::chrono::milliseconds{std::numeric_limits<int>::max()} + std::chrono::milliseconds{1});
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, HeaderBodyLimits) {
  HttpServerConfig cfg;
  cfg.maxHeaderBytes = 10;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
  cfg.maxHeaderBytes = 1024;
  cfg.maxBodyBytes = 0;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, WithDecompressionConfig) {
  HttpServerConfig config;
  DecompressionConfig decfg;
  decfg.enable = true;
  decfg.decoderChunkSize = 1024;
  decfg.maxDecompressedBytes = 10UL * 1024 * 1024;
  config.withRequestDecompression(decfg);
  EXPECT_NO_THROW(config.validate());

  EXPECT_EQ(config.decompression.enable, true);
  EXPECT_EQ(config.decompression.decoderChunkSize, 1024U);
  EXPECT_EQ(config.decompression.maxDecompressedBytes, 10UL * 1024 * 1024);
}

}  // namespace aeronet