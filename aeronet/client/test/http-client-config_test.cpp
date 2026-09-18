#include "aeronet/http-client-config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "aeronet/client-protocol.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/retry-config.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-config.hpp"
#endif

namespace aeronet {

TEST(HttpClientConfigTest, WithGlobalHeadersShouldReplaceAllList) {
  HttpClientConfig config;
  config.withGlobalHeaders(vector<http::Header>{http::Header{"X-Valid", "value"}, http::Header{"X-Custom", "value"}});
  EXPECT_EQ(config.globalHeaders.nbConcatenatedStrings(), 2U);
  config.withGlobalHeaders(vector<http::Header>{http::Header{"X-Valid2", "value"}, http::Header{"X-Custom2", "value"}});
  EXPECT_EQ(config.globalHeaders.nbConcatenatedStrings(), 2U);
  config.withGlobalHeaders({});
  EXPECT_TRUE(config.globalHeaders.empty());

  EXPECT_NO_THROW(config.validate());
}

TEST(HttpClientConfigTest, InvalidRequestTimeout) {
  HttpClientConfig config;
  config.requestTimeout = std::chrono::milliseconds{0};
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, KeepAliveTimeout) {
  HttpClientConfig config;
  config.keepAliveTimeout = std::chrono::milliseconds{0};
  EXPECT_NO_THROW(config.validate());

  config.keepAliveTimeout = std::chrono::milliseconds{-1};
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, InvalidMaxResponseBytes) {
  HttpClientConfig config;
  config.maxResponseBytes = 0;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, InvalidGlobalHeaderValueWithControlChars) {
  HttpClientConfig config;
  config.globalHeaders.append("X-Test:value\x01");  // control char 0x01
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, Http2VersionValidation) {
  HttpClientConfig config;
  config.httpVersion = HttpVersionMode::Http2;
  config.globalHeaders.append("X-Test:value\x01");  // control char 0x01
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, NoGlobalHeadersIsValidAndShouldNotAddDefaultOnes) {
  HttpClientConfig config;
  config.withGlobalHeaders({});
  EXPECT_TRUE(config.globalHeaders.empty());
  EXPECT_NO_THROW(config.validate());
  EXPECT_TRUE(config.globalHeaders.empty());
}

TEST(HttpClientConfigTest, AddGlobalHeader) {
  HttpClientConfig config;
  config.addGlobalHeader(http::Header{"X-Test", "value"});
  EXPECT_NO_THROW(config.validate());
}

TEST(HttpClientConfigTest, HeaderKey1) {
  HttpClientConfig config;
  config.withGlobalHeaders(vector<http::Header>{http::Header{"X-Valid", "value"}, http::Header{"X-Custom", "value"}});

  EXPECT_NO_THROW(config.validate());
}

TEST(HttpClientConfigTest, HeaderKey2) {
  HttpClientConfig config;
  config.globalHeaders.append(":value");

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, HeaderKey3) {
  HttpClientConfig config;
  config.globalHeaders.append("Invalid Char!: value");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, HeaderKey4) {
  HttpClientConfig config;
  config.globalHeaders.append("Another@Invalid: value");  // invalid char '@'
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, ReservedGlobalHeaderShouldThrow) {
  HttpClientConfig config;
  config.globalHeaders.append("Content-Length: 10");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, InvalidGlobalHeaderShouldThrow1) {
  HttpClientConfig config;
  config.globalHeaders.append("Invalid\nHeader: value");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, InvalidGlobalHeaderShouldThrow2) {
  HttpClientConfig config;
  config.globalHeaders.append("X-Custom: value\x7F");  // DEL control char
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, InvalidGlobalHeaderShouldThrow3) {
  HttpClientConfig config;
  config.globalHeaders.append("X-Cust:om: value");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, GlobalHeaderShouldContainHeaderSep1) {
  HttpClientConfig config;
  config.globalHeaders.append("InvalidNoColon");
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, GlobalHeaderShouldContainHeaderSep2) {
  HttpClientConfig config;
  config.globalHeaders.append("X-Custom:value");
  ASSERT_FALSE(config.globalHeaders.contains(http::HeaderSep));
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, MinReadChunkBytesCannotBeZero) {
  HttpClientConfig config;
  config.minReadChunkBytes = 0;

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, DefaultsAreSane) {
  HttpClientConfig cfg;
  EXPECT_TRUE(cfg.defaultAcceptEncoding().empty());
  EXPECT_TRUE(cfg.followRedirects);
  EXPECT_TRUE(cfg.keepAlive);
  EXPECT_EQ(cfg.tcpNoDelay, TcpNoDelayMode::Auto);
  EXPECT_FALSE(cfg.requestCompression.enabled());
  EXPECT_EQ(cfg.requestCompression.encoding, Encoding::none);  // none == disabled (single source of truth)
  // decompression auto-enables whenever at least one decoder is compiled in.
  EXPECT_EQ(cfg.decompression.enable,
            IsEncodingEnabled(Encoding::zstd) || IsEncodingEnabled(Encoding::br) || IsEncodingEnabled(Encoding::gzip));
  EXPECT_EQ(cfg.minCapturedBodySize, 1024UL);
  // Default retry policy keeps the historical behaviour: only the free pre-send stale-pool retry.
  EXPECT_EQ(cfg.retry.maxAttempts, 1U);
}

TEST(HttpClientConfigTest, WithRetryBuilder) {
  RetryConfig retry;
  retry.maxAttempts = 4;
  retry.retryIdempotentAfterSend = true;
  HttpClientConfig cfg;
  cfg.withRetry(retry);
  EXPECT_EQ(cfg.retry.maxAttempts, 4U);
  EXPECT_TRUE(cfg.retry.retryIdempotentAfterSend);
}

TEST(HttpClientConfigTest, FluentSettersAreChainable) {
  HttpClientConfig cfg;
  cfg.withDefaultAcceptEncoding("gzip")
      .withTcpNoDelayMode(TcpNoDelayMode::Disabled)
      .withKeepAliveTimeout(std::chrono::seconds{5})
      .withMinCapturedBodySize(2048);
  EXPECT_EQ(cfg.defaultAcceptEncoding(), "gzip");
  EXPECT_EQ(cfg.tcpNoDelay, TcpNoDelayMode::Disabled);
  EXPECT_EQ(cfg.keepAliveTimeout, std::chrono::seconds{5});
  EXPECT_EQ(cfg.minCapturedBodySize, 2048U);
}

TEST(HttpClientConfigTest, TcpNoDelayBoolHelper) {
  HttpClientConfig cfg;
  cfg.withTcpNoDelay(false);
  EXPECT_EQ(cfg.tcpNoDelay, TcpNoDelayMode::Disabled);
  cfg.withTcpNoDelay();
  EXPECT_EQ(cfg.tcpNoDelay, TcpNoDelayMode::Enabled);
}

TEST(HttpClientConfigTest, DecompressionHelper) {
  HttpClientConfig cfg;
  cfg.withDecompression(false);
  EXPECT_FALSE(cfg.decompression.enable);
  cfg.withDecompression();
  EXPECT_TRUE(cfg.decompression.enable);
}

TEST(HttpClientConfigTest, RequestCompressionHelpers) {
  HttpClientConfig cfg;
  cfg.withRequestCompression(true);  // enable with the default compiled-in codec
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  EXPECT_TRUE(cfg.requestCompression.enabled());
#else
  EXPECT_FALSE(cfg.requestCompression.enabled());
#endif
  EXPECT_EQ(cfg.requestCompression.encoding, internal::DefaultRequestEncoding());

  cfg.withRequestCompression(Encoding::gzip);  // selects a specific codec
  EXPECT_TRUE(cfg.requestCompression.enabled());
  EXPECT_EQ(cfg.requestCompression.encoding, Encoding::gzip);

  cfg.withRequestCompression(false);  // disabling clears the codec back to none
  EXPECT_FALSE(cfg.requestCompression.enabled());
  EXPECT_EQ(cfg.requestCompression.encoding, Encoding::none);
}

TEST(HttpClientConfigTest, DefaultRequestEncodingMatchesBuild) {
  // The default request codec is the first compiled-in coding in aeronet's preference order.
  Encoding expected = Encoding::none;
  if (IsEncodingEnabled(Encoding::zstd)) {
    expected = Encoding::zstd;
  } else if (IsEncodingEnabled(Encoding::br)) {
    expected = Encoding::br;
  } else if (IsEncodingEnabled(Encoding::gzip)) {
    expected = Encoding::gzip;
  }
  EXPECT_EQ(internal::DefaultRequestEncoding(), expected);
  // Request compression is opt-in: the unset encoding is none, and enabling picks the default codec.
  EXPECT_EQ(HttpClientConfig{}.requestCompression.encoding, Encoding::none);
  EXPECT_EQ(HttpClientConfig{}.withRequestCompression().requestCompression.encoding, expected);
}

TEST(HttpClientConfigTest, Validate) {
  HttpClientConfig cfg;
  EXPECT_NO_THROW(cfg.validate());
  cfg.connectTimeout = std::chrono::milliseconds{0};
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
  cfg.connectTimeout = std::chrono::milliseconds{std::numeric_limits<int>::max() + 1ULL};
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
  cfg.connectTimeout = std::chrono::milliseconds{1};
  EXPECT_NO_THROW(cfg.validate());
}

#ifdef AERONET_ENABLE_OPENSSL
TEST(HttpClientConfigTest, TlsSettersAndGetters) {
  HttpClientConfig cfg;
  cfg.withTlsCaFile("/tmp/ca.pem")
      .withTlsCaPath("/tmp/ca")
      .withTlsCipherList("HIGH")
      .withTlsClientCertKeyFile("/tmp/cert.pem", "/tmp/key.pem")
      .withTlsMinVersion(TLSConfig::TLS_1_2)
      .withTlsMaxVersion(TLSConfig::TLS_1_3);
  EXPECT_EQ(cfg.tlsCaFile(), "/tmp/ca.pem");
  EXPECT_STREQ(cfg.tlsCaFileCStr(), "/tmp/ca.pem");
  EXPECT_EQ(cfg.tlsCaPath(), "/tmp/ca");
  EXPECT_STREQ(cfg.tlsCaPathCStr(), "/tmp/ca");
  EXPECT_EQ(cfg.tlsCipherList(), "HIGH");
  EXPECT_STREQ(cfg.tlsCipherListCStr(), "HIGH");
  EXPECT_EQ(cfg.tlsClientCertFile(), "/tmp/cert.pem");
  EXPECT_STREQ(cfg.tlsClientCertFileCStr(), "/tmp/cert.pem");
  EXPECT_EQ(cfg.tlsClientKeyFile(), "/tmp/key.pem");
  EXPECT_STREQ(cfg.tlsClientKeyFileCStr(), "/tmp/key.pem");
  EXPECT_EQ(cfg.tlsMinVersion, TLSConfig::TLS_1_2);
  EXPECT_EQ(cfg.tlsMaxVersion, TLSConfig::TLS_1_3);
}

TEST(HttpClientConfigTest, TlsInMemoryClientCert) {
  HttpClientConfig cfg;
  cfg.withTlsClientCertKeyMemory("CERT-PEM", "KEY-PEM");
  EXPECT_EQ(cfg.tlsClientCertPem(), "CERT-PEM");
  EXPECT_EQ(cfg.tlsClientKeyPem(), "KEY-PEM");
}
#endif

TEST(HttpClientConfigTest, HttpVersionDefaultsToAuto) {
  HttpClientConfig cfg;
  EXPECT_EQ(cfg.httpVersion, HttpVersionMode::Auto);
}

TEST(HttpClientConfigTest, WithHttpVersionAndHttp2ConfigBuilders) {
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2).withHttp2Config(Http2Config{}.withMaxConcurrentStreams(7));
  EXPECT_EQ(cfg.httpVersion, HttpVersionMode::Http2);
  EXPECT_EQ(cfg.http2.maxConcurrentStreams, 7U);
}

TEST(HttpClientConfigTest, ValidateChecksHttp2SettingsWhenHttp2IsPossible) {
  HttpClientConfig cfg;
  cfg.http2.maxFrameSize = 1;  // below the RFC 9113 minimum
  // HTTP/1.1-only clients never consult the HTTP/2 settings, so validation ignores them.
  cfg.withHttpVersion(HttpVersionMode::Http1_1);
  EXPECT_NO_THROW(cfg.validate());
#ifdef AERONET_ENABLE_HTTP2
  cfg.withHttpVersion(HttpVersionMode::Auto);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
  cfg.withHttpVersion(HttpVersionMode::Http2);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
#else
  // Requiring HTTP/2 in a build without the engine is rejected outright.
  cfg.withHttpVersion(HttpVersionMode::Http2);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
#endif
}

TEST(HttpClientConfigTest, ValidateRejectsHttp2Push) {
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2).withHttp2Config(Http2Config{}.withEnablePush(true));
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, ProxyDefaultsToDisabled) {
  HttpClientConfig cfg;
  EXPECT_FALSE(cfg.hasProxy());
  EXPECT_TRUE(cfg.proxyUrl().empty());
  EXPECT_TRUE(cfg.proxyCaFile().empty());
}

TEST(HttpClientConfigTest, WithProxyUrlOnly) {
  HttpClientConfig cfg;
  cfg.withProxy("http://127.0.0.1:8080");
  EXPECT_TRUE(cfg.hasProxy());
  EXPECT_EQ(cfg.proxyUrl(), "http://127.0.0.1:8080");
  EXPECT_TRUE(cfg.proxyCaFile().empty());
}

TEST(HttpClientConfigTest, WithProxyUrlAndCaFile) {
  HttpClientConfig cfg;
  cfg.withProxy("http://proxy.local:3128", "/etc/mitmproxy/ca.pem");
  EXPECT_TRUE(cfg.hasProxy());
  EXPECT_EQ(cfg.proxyUrl(), "http://proxy.local:3128");
  EXPECT_EQ(cfg.proxyCaFile(), "/etc/mitmproxy/ca.pem");
  EXPECT_STREQ(cfg.proxyCaFileCStr(), "/etc/mitmproxy/ca.pem");
}

TEST(HttpClientConfigTest, CacheDefaultsDisabled) {
  HttpClientConfig cfg;
  EXPECT_FALSE(cfg.cache.enabled());
  EXPECT_EQ(cfg.cache.maxEntries, 1024U);
  EXPECT_EQ(cfg.cache.methods, http::Method::GET | http::Method::HEAD);
}

TEST(HttpClientConfigTest, CacheBuildersSetFields) {
  HttpClientConfig cfg;
  cfg.withCache(std::chrono::seconds{5}).withCacheMaxEntries(32).withCacheMethods(http::Method::GET);
  EXPECT_TRUE(cfg.cache.enabled());
  EXPECT_EQ(cfg.cache.refreshPeriod, std::chrono::seconds{5});
  EXPECT_EQ(cfg.cache.maxEntries, 32U);
  EXPECT_EQ(cfg.cache.methods, static_cast<http::MethodBmp>(http::Method::GET));
}

TEST(HttpClientConfigTest, CacheValidateRejectsUnsafeMethods) {
  HttpClientConfig cfg;
  cfg.withCache(std::chrono::seconds{5}).withCacheMethods(http::Method::GET | http::Method::POST);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, CacheValidateRejectsEmptyMethods) {
  HttpClientConfig cfg;
  cfg.withCache(std::chrono::seconds{5}).withCacheMethods(0);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, CacheValidateRejectsZeroMaxEntries) {
  HttpClientConfig cfg;
  cfg.withCache(std::chrono::seconds{5}).withCacheMaxEntries(0);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, CacheValidationSkippedWhenDisabled) {
  HttpClientConfig cfg;                            // cache disabled by default (zero refresh period)
  cfg.withCacheMaxEntries(0).withCacheMethods(0);  // nonsensical, but ignored while the cache is off
  EXPECT_NO_THROW(cfg.validate());
}

}  // namespace aeronet