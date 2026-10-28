#include "aeronet/http-server-config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "aeronet/decompression-config.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/zerocopy-mode.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-config.hpp"
#endif

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

TEST(HttpServerConfigTest, NoGlobalHeadersIsValidAndShouldNotAddDefaultOnes) {
  HttpServerConfig config;
  config.withGlobalHeaders({});
  EXPECT_TRUE(config.globalHeaders.empty());
  EXPECT_NO_THROW(config.validate());
  EXPECT_TRUE(config.globalHeaders.empty());
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
  config.globalHeaders.append("Invalid Char!: value");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, HeaderKey4) {
  HttpServerConfig config;
  config.globalHeaders.append("Another@Invalid: value");  // invalid char '@'
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, HeaderValueShouldBeTrimmed) {
  HttpServerConfig config;
  config.globalHeaders.append("X-Test: value ");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, ReservedGlobalHeaderShouldThrow) {
  HttpServerConfig config;
  config.globalHeaders.append("Content-Length: 10");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, InvalidGlobalHeaderShouldThrow1) {
  HttpServerConfig config;
  config.globalHeaders.append("Invalid\nHeader: value");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, InvalidGlobalHeaderShouldThrow2) {
  HttpServerConfig config;
  config.globalHeaders.append("X-Custom: value\x7F");  // DEL control char
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, InvalidGlobalHeaderShouldThrow3) {
  HttpServerConfig config;
  config.globalHeaders.append("X-Cust:om: value");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, GlobalHeaderShouldContainHeaderSep1) {
  HttpServerConfig config;
  config.globalHeaders.append("InvalidNoColon");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, GlobalHeaderShouldContainHeaderSep2) {
  HttpServerConfig config;
  config.globalHeaders.append("X-Custom:value");
  ASSERT_FALSE(config.globalHeaders.contains(http::HeaderSep));
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

// The value of a Location header is specific to each response, and the redirects of the server are built on a response
// that already starts with the global headers: a global one would be duplicated, and followed first by the clients.
TEST(HttpServerConfigTest, LocationAsGlobalHeaderShouldThrow) {
  for (std::string_view name : {"location", "Location", "LOCATION"}) {
    SCOPED_TRACE(name);

    HttpServerConfig config;
    config.addGlobalHeader(http::Header{name, "/home"});
    EXPECT_THROW(config.validate(), std::invalid_argument);

    // Among other valid headers.
    HttpServerConfig other;
    other.addGlobalHeader(http::Header{"X-Before", "1"});
    other.addGlobalHeader(http::Header{name, "/home"});
    other.addGlobalHeader(http::Header{"X-After", "2"});
    EXPECT_THROW(other.validate(), std::invalid_argument);

    HttpServerConfig appended;
    appended.globalHeaders.append(std::string(name) + ": /home");
    EXPECT_THROW(appended.validate(), std::invalid_argument);
  }
}

TEST(HttpServerConfigTest, LocationAsGlobalHeaderErrorMentionsTheHeader) {
  HttpServerConfig config;
  config.addGlobalHeader(http::Header{"Location", "/home"});
  try {
    config.validate();
    FAIL() << "validate() should throw for a global location header";
  } catch (const std::invalid_argument& ex) {
    EXPECT_TRUE(std::string_view(ex.what()).contains("location")) << ex.what();
  }
}

// Only the exact name is rejected.
TEST(HttpServerConfigTest, HeadersWithLocationInTheirNameAreValidGlobalHeaders) {
  HttpServerConfig config;
  config.addGlobalHeader(http::Header{"Content-Location", "/home"});
  config.addGlobalHeader(http::Header{"X-Location", "/home"});
  config.addGlobalHeader(http::Header{"Locations", "/home"});
  EXPECT_NO_THROW(config.validate());
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

TEST(HttpServerConfigTest, MaxPerEventReadBytesMustBePositive) {
  HttpServerConfig cfg;
  cfg.withMinReadChunkBytes(1024);

  EXPECT_THROW(cfg.withMaxPerEventReadBytes(0), std::invalid_argument);

  cfg.withMaxPerEventReadBytes(1024);
  EXPECT_NO_THROW(cfg.validate());

  EXPECT_THROW(cfg.withMaxPerEventReadBytes(1ULL << 34U), std::invalid_argument);
}

TEST(HttpServerConfig, MinReadChunkBytes) {
  HttpServerConfig cfg;
  cfg.withMinReadChunkBytes(0);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
  EXPECT_THROW(cfg.withMinReadChunkBytes(1ULL << 34U), std::invalid_argument);
}

TEST(HttpServerConfig, MinReadChunkBytesGreaterThanMaxPerEventReadBytes) {
  HttpServerConfig cfg;
  cfg.withMinReadChunkBytes(2048);
  cfg.withMaxPerEventReadBytes(1024);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpServerConfig, ComputeReadChunkSizeReadsRemainingBudgetWithinReadSize) {
  HttpServerConfig cfg;
  cfg.withMinReadChunkBytes(4096).withMaxPerEventReadBytes(128UL << 10U);
  // A new connection reads minReadChunkBytes.
  EXPECT_EQ(cfg.computeReadChunkSize(0, 0), 4096U);
  EXPECT_EQ(cfg.computeReadChunkSize(0, 3), 32UL << 10U);
  EXPECT_EQ(cfg.computeReadChunkSize(0, 5), 128UL << 10U);
  // A read size beyond the remaining budget reads the remaining budget.
  EXPECT_EQ(cfg.computeReadChunkSize(0, 6), 128UL << 10U);
  EXPECT_EQ(cfg.computeReadChunkSize(100UL << 10U, 5), 28UL << 10U);
  EXPECT_EQ(cfg.computeReadChunkSize((128UL << 10U) - 4096U, 5), 4096U);
  // An almost exhausted budget still reads minReadChunkBytes.
  EXPECT_EQ(cfg.computeReadChunkSize((128UL << 10U) - 4095U, 5), 4096U);
  EXPECT_EQ(cfg.computeReadChunkSize((128UL << 10U) - 1U, 0), 4096U);
}

TEST(HttpServerConfig, ReadSizeIsCappedForLargeBudgets) {
  static constexpr std::size_t kMaxChunk = HttpServerConfig::kMaxReadChunkBytes;
  HttpServerConfig cfg;
  cfg.withMinReadChunkBytes(4096).withMaxPerEventReadBytes(std::numeric_limits<std::uint32_t>::max());
  EXPECT_EQ(cfg.readSize(0), 4096U);
  EXPECT_EQ(cfg.readSize(6), kMaxChunk);
  EXPECT_EQ(cfg.readSize(7), kMaxChunk);
  EXPECT_EQ(cfg.computeReadChunkSize(0, 6), kMaxChunk);
  EXPECT_EQ(cfg.computeReadChunkSize(1UL << 30U, 6), kMaxChunk);
  EXPECT_EQ(cfg.computeReadChunkSize(std::numeric_limits<std::uint32_t>::max() - kMaxChunk + 1U, 6), kMaxChunk - 1U);
  EXPECT_EQ(cfg.computeReadChunkSize(std::numeric_limits<std::uint32_t>::max() - 10U, 6), 4096U);

  // A minReadChunkBytes that is not a power of 2 is doubled up to kMaxReadChunkBytes exactly.
  cfg.withMinReadChunkBytes(5000);
  EXPECT_EQ(cfg.readSize(5), 5000UL << 5U);
  EXPECT_EQ(cfg.readSize(6), kMaxChunk);

  // A minReadChunkBytes above the cap is honored.
  cfg.withMinReadChunkBytes(2 * kMaxChunk).withMaxPerEventReadBytes(4 * kMaxChunk);
  EXPECT_NO_THROW(cfg.validate());
  EXPECT_EQ(cfg.readSize(0), 2 * kMaxChunk);
  EXPECT_EQ(cfg.computeReadChunkSize(0, 0), 2 * kMaxChunk);
  EXPECT_EQ(cfg.computeReadChunkSize(3 * kMaxChunk, 0), 2 * kMaxChunk);
}

TEST(HttpServerConfig, AdaptReadSizeShift) {
  HttpServerConfig cfg;
  cfg.withMinReadChunkBytes(4096).withMaxPerEventReadBytes(1UL << 20U);
  // A read filling the read size doubles it, up to kMaxReadChunkBytes.
  EXPECT_EQ(cfg.adaptReadSizeShift(0, 4096, 4096), 1U);
  EXPECT_EQ(cfg.adaptReadSizeShift(5, 128UL << 10U, 128UL << 10U), 6U);
  EXPECT_EQ(cfg.adaptReadSizeShift(6, HttpServerConfig::kMaxReadChunkBytes, HttpServerConfig::kMaxReadChunkBytes), 6U);
  // A full read cut by the budget, or raised to minReadChunkBytes at its end, keeps it.
  EXPECT_EQ(cfg.adaptReadSizeShift(5, 64UL << 10U, 64UL << 10U), 5U);
  EXPECT_EQ(cfg.adaptReadSizeShift(5, 4096, 4096), 5U);
  // A read filling at least a quarter of the requested size keeps it.
  EXPECT_EQ(cfg.adaptReadSizeShift(3, 32UL << 10U, (32UL << 10U) - 1U), 3U);
  EXPECT_EQ(cfg.adaptReadSizeShift(3, 32UL << 10U, 8UL << 10U), 3U);
  // A read filling less than a quarter of it halves it, down to minReadChunkBytes.
  EXPECT_EQ(cfg.adaptReadSizeShift(3, 32UL << 10U, (8UL << 10U) - 1U), 2U);
  EXPECT_EQ(cfg.adaptReadSizeShift(3, 4096, 1), 2U);
  EXPECT_EQ(cfg.adaptReadSizeShift(0, 4096, 1), 0U);

  // A minReadChunkBytes at the cap never grows.
  cfg.withMinReadChunkBytes(HttpServerConfig::kMaxReadChunkBytes);
  EXPECT_EQ(cfg.adaptReadSizeShift(0, HttpServerConfig::kMaxReadChunkBytes, HttpServerConfig::kMaxReadChunkBytes), 0U);
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

TEST(HttpServerConfigTest, ExcessiveHeaderReadTimeoutThrows) {
  HttpServerConfig config;
  config.withHeaderReadTimeout(std::chrono::milliseconds{std::chrono::hours{24 * 50}});  // 50 days > ~49.7 days
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, ExcessiveBodyReadTimeoutThrows) {
  HttpServerConfig config;
  config.withBodyReadTimeout(std::chrono::milliseconds{std::chrono::hours{24 * 50}});
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, InvalidLingeringCloseTimeoutThrows) {
  HttpServerConfig config;
  config.withLingeringCloseTimeout(std::chrono::milliseconds{-1});
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, ExcessiveLingeringCloseTimeoutThrows) {
  HttpServerConfig config;
  config.withLingeringCloseTimeout(std::chrono::milliseconds{std::chrono::hours{24 * 50}});
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, ExcessiveKeepAliveTimeoutThrows) {
  HttpServerConfig config;
  config.withKeepAliveTimeout(std::chrono::milliseconds{std::chrono::hours{24 * 50}});
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, LargeButValidTimeoutsPass) {
  HttpServerConfig config;
  // Just under 49 days should be fine
  config.withHeaderReadTimeout(std::chrono::milliseconds{std::chrono::hours{24 * 49}});
  config.withBodyReadTimeout(std::chrono::milliseconds{std::chrono::hours{24 * 49}});
  config.withKeepAliveTimeout(std::chrono::milliseconds{std::chrono::hours{24 * 49}});
  config.withLingeringCloseTimeout(std::chrono::milliseconds{std::chrono::hours{24 * 49}});
  EXPECT_NO_THROW(config.validate());
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

TEST(HttpServerConfigTest, AdaptivePollTimeoutValidPolicy) {
  HttpServerConfig config;
  config.withPollInterval(std::chrono::milliseconds{10}).withPollIntervalFactors(0.0F, 8.0F);

  EXPECT_EQ(config.pollIntervalMinFactor, 0.0F);
  EXPECT_EQ(config.pollIntervalMaxFactor, 8.0F);
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, AdaptivePollTimeoutValidationRejectsInvalidPolicy) {
  {
    HttpServerConfig config;
    config.withPollIntervalFactors(-0.1F, 1.0F);
    EXPECT_THROW(config.validate(), std::invalid_argument);
  }
  {
    HttpServerConfig config;
    config.withPollIntervalFactors(1.1F, 2.0F);
    EXPECT_THROW(config.validate(), std::invalid_argument);
  }
  {
    HttpServerConfig config;
    config.withPollIntervalFactors(0.5F, 0.9F);
    EXPECT_THROW(config.validate(), std::invalid_argument);
  }
  {
    HttpServerConfig config;
    config.withPollIntervalFactors(std::numeric_limits<float>::quiet_NaN(), 2.0F);
    EXPECT_THROW(config.validate(), std::invalid_argument);
  }
  {
    HttpServerConfig config;
    config.withPollIntervalFactors(0.0F, std::numeric_limits<float>::infinity());
    EXPECT_THROW(config.validate(), std::invalid_argument);
  }
  {
    HttpServerConfig config;
    config.withPollInterval(std::chrono::milliseconds{std::numeric_limits<int>::max() / 2})
        .withPollIntervalFactors(0.0F, 3.0F);
    EXPECT_THROW(config.validate(), std::invalid_argument);
  }
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
#if defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZSTD)
  EXPECT_NO_THROW(config.validate());
#else
  EXPECT_THROW(config.validate(), std::invalid_argument);
#endif

  EXPECT_EQ(config.decompression.enable, true);
  EXPECT_EQ(config.decompression.decoderChunkSize, 1024U);
  EXPECT_EQ(config.decompression.maxDecompressedBytes, 10UL * 1024 * 1024);
}

TEST(HttpServerConfigTest, CachedConnections) {
  HttpServerConfig config;
  config.withMaxCachedConnections(0);
  EXPECT_EQ(config.maxCachedConnections, 0U);
  EXPECT_NO_THROW(config.validate());

  config.withMaxCachedConnections(100);
  EXPECT_EQ(config.maxCachedConnections, 100U);
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, MaxAcceptBatchSizeShouldNotBeZero) {
  HttpServerConfig config;
  config.withMaxAcceptBatchSize(0);
  EXPECT_EQ(config.maxAcceptBatchSize, 0U);
  EXPECT_THROW(config.validate(), std::invalid_argument);  // 0 is not a valid value for maxAcceptBatchSize
}

TEST(HttpServerConfigTest, ZeroCopyMinBytesShouldNotBeZero) {
  HttpServerConfig config;
  config.zerocopyMinBytes = 0;

  EXPECT_THROW(config.validate(), std::invalid_argument);

  config.zerocopyMinBytes = 1;
  config.validate();

  config.zerocopyMinBytes = 0;
  config.withZerocopyMode(ZerocopyMode::Disabled);
  config.validate();
}

TEST(HttpServerConfigTest, MaxPerEventReadBytesShouldNotBeZero) {
  HttpServerConfig config;
  EXPECT_THROW(config.withMaxPerEventReadBytes(0), std::invalid_argument);
  config.maxPerEventReadBytes = 0;
  EXPECT_EQ(config.maxPerEventReadBytes, 0U);
  EXPECT_THROW(config.validate(), std::invalid_argument);  // 0 is not a valid value for maxPerEventReadBytes
}

// ============================
// Zerocopy configuration tests
// ============================

TEST(HttpServerConfigTest, ZerocopyModeDefault) {
  HttpServerConfig config;
  // Default should be Opportunistic
  EXPECT_EQ(config.zerocopyMode, ZerocopyMode::Opportunistic);
  EXPECT_EQ(config.maxZerocopyPendingBytes, 4U << 20U);
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, ZerocopyPendingLimit) {
  HttpServerConfig config;
  config.withMaxZerocopyPendingBytes(8U << 20U);
  EXPECT_EQ(config.maxZerocopyPendingBytes, 8U << 20U);
  EXPECT_NO_THROW(config.validate());

  config.withMaxZerocopyPendingBytes(1023);
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, ZerocopyModeDisabled) {
  HttpServerConfig config;
  config.withZerocopyMode(ZerocopyMode::Disabled);
  EXPECT_EQ(config.zerocopyMode, ZerocopyMode::Disabled);
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, ZerocopyModeOpportunistic) {
  HttpServerConfig config;
  config.withZerocopyMode(ZerocopyMode::Opportunistic);
  EXPECT_EQ(config.zerocopyMode, ZerocopyMode::Opportunistic);
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, ZerocopyModeEnabled) {
  HttpServerConfig config;
  config.withZerocopyMode(ZerocopyMode::Enabled);
  EXPECT_EQ(config.zerocopyMode, ZerocopyMode::Enabled);
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, ZerocopyModeBuilderChaining) {
  // Verify builder pattern chaining works correctly
  HttpServerConfig config = HttpServerConfig{}.withPort(8080).withZerocopyMode(ZerocopyMode::Enabled).withTcpNoDelay();

  EXPECT_EQ(config.port, 8080U);
  EXPECT_EQ(config.zerocopyMode, ZerocopyMode::Enabled);
  EXPECT_EQ(config.tcpNoDelay, TcpNoDelayMode::Enabled);
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, WithTcpNoDelayMode) {
  HttpServerConfig config;
  config.withTcpNoDelay(true);
  EXPECT_EQ(config.tcpNoDelay, TcpNoDelayMode::Enabled);
  EXPECT_NO_THROW(config.validate());

  config.withTcpNoDelay(false);
  EXPECT_EQ(config.tcpNoDelay, TcpNoDelayMode::Disabled);
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, HttpsRedirectDefaultsDisabled) {
  HttpServerConfig config;
  EXPECT_FALSE(config.httpsRedirect.enabled());
  EXPECT_EQ(config.httpsRedirect.targetPort, 0);
  EXPECT_EQ(config.httpsRedirect.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, WithHttpsRedirectDefaultArgs) {
  HttpServerConfig config;
  config.withHttpsRedirect();
  EXPECT_TRUE(config.httpsRedirect.enabled());
  EXPECT_EQ(config.httpsRedirect.targetPort, 443);
  EXPECT_EQ(config.httpsRedirect.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, WithHttpsRedirectBuilder) {
  HttpServerConfig config;

  for (http::StatusCode statusCode : {
           http::StatusCodeMovedPermanently,
           http::StatusCodeFound,
           http::StatusCodeTemporaryRedirect,
           http::StatusCodePermanentRedirect,
       }) {
    config.withHttpsRedirect(8443, statusCode);
    EXPECT_TRUE(config.httpsRedirect.enabled());
    EXPECT_EQ(config.httpsRedirect.targetPort, 8443);
    EXPECT_EQ(config.httpsRedirect.statusCode, statusCode);
    EXPECT_NO_THROW(config.validate());
  }
}

TEST(HttpServerConfigTest, HttpsRedirectZeroPortDisablesAndValidates) {
  HttpServerConfig config;
  config.withHttpsRedirect(0);  // 0 disables the redirect entirely (no error)
  EXPECT_FALSE(config.httpsRedirect.enabled());
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpServerConfigTest, HttpsRedirectRejectsNonRedirectStatus) {
  HttpServerConfig config;
  config.withHttpsRedirect(443, http::StatusCodeOK);
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpServerConfigTest, HttpsRedirectRejectsCombinationWithTls) {
  HttpServerConfig config;
  config.withHttpsRedirect();
  config.tls.enabled = true;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

#ifdef AERONET_ENABLE_OPENSSL
TEST(HttpServerConfigTest, InvalidCipherListThrows) {
  HttpServerConfig cfg;
  cfg.withTlsCipherList("THIS-IS-NOT-A-REAL-CIPHER");  // matches no cipher -> SSL_CTX_set_cipher_list fails
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}
#endif

TEST(HttpServerConfigTest, ConnectAllowListCannotContainStarWithOtherHosts) {
  HttpServerConfig config;

  static constexpr std::string_view allowedHosts[]{"www.example.com", "www.example2.com"};
  config.withConnectAllowlist(std::begin(allowedHosts), std::end(allowedHosts));

  EXPECT_NO_THROW(config.validate());

  static constexpr std::string_view allowedHostsUnrestricted[]{"*"};
  config.withConnectAllowlist(std::begin(allowedHostsUnrestricted), std::end(allowedHostsUnrestricted));

  EXPECT_NO_THROW(config.validate());

  static constexpr std::string_view allowedHostsUnrestrictedWithOthers[]{"*", "www.example.com"};
  config.withConnectAllowlist(std::begin(allowedHostsUnrestrictedWithOthers),
                              std::end(allowedHostsUnrestrictedWithOthers));

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

#ifdef AERONET_ENABLE_HTTP2
TEST(HttpServerConfigTest, WithHttp2Config) {
  HttpServerConfig config;
  Http2Config http2cfg;
  http2cfg.enable = true;
  http2cfg.maxConcurrentStreams = 100;
  http2cfg.initialWindowSize = 65536;
  config.withHttp2(http2cfg);
  EXPECT_NO_THROW(config.validate());

  config.enableHttp2(false);
  EXPECT_FALSE(config.http2.enable);
}
#endif

}  // namespace aeronet
