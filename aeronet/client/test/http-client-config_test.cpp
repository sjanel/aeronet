// Unit coverage for HttpClientConfig: the fluent setters, the packed-string getters, and the new
// compression / decompression knobs. No sockets involved.
#include "aeronet/http-client-config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <stdexcept>

#include "aeronet/encoding.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/retry-config.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"

namespace aeronet {

TEST(HttpClientConfigTest, DefaultsAreSane) {
  HttpClientConfig cfg;
  EXPECT_EQ(cfg.userAgent(), "aeronet-client");
  EXPECT_TRUE(cfg.defaultAcceptEncoding().empty());
  EXPECT_TRUE(cfg.followRedirects);
  EXPECT_TRUE(cfg.keepAlive);
  EXPECT_EQ(cfg.tcpNoDelay, TcpNoDelayMode::Auto);
  EXPECT_FALSE(cfg.requestCompression.enabled());
  EXPECT_EQ(cfg.requestCompression.encoding, Encoding::none);  // none == disabled (single source of truth)
  // decompression auto-enables whenever at least one decoder is compiled in.
  EXPECT_EQ(cfg.decompression.enable,
            IsEncodingEnabled(Encoding::zstd) || IsEncodingEnabled(Encoding::br) || IsEncodingEnabled(Encoding::gzip));
  EXPECT_EQ(cfg.maxCapturedRequestBodyBytes, 8UL * 1024UL);
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
  cfg.withUserAgent("my-agent/2.0")
      .withDefaultAcceptEncoding("gzip")
      .withTcpNoDelayMode(TcpNoDelayMode::Disabled)
      .withKeepAliveTimeout(std::chrono::seconds{5})
      .withMaxCapturedRequestBodyBytes(2048);
  EXPECT_EQ(cfg.userAgent(), "my-agent/2.0");
  EXPECT_EQ(cfg.defaultAcceptEncoding(), "gzip");
  EXPECT_EQ(cfg.tcpNoDelay, TcpNoDelayMode::Disabled);
  EXPECT_EQ(cfg.keepAliveTimeout, std::chrono::seconds{5});
  EXPECT_EQ(cfg.maxCapturedRequestBodyBytes, 2048U);
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
  EXPECT_TRUE(cfg.requestCompression.enabled());
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

TEST(HttpClientConfigTest, ConstructionValidatesCodecConfig) {
  // Valid default config constructs fine and exposes its config back.
  HttpClient ok;
  EXPECT_EQ(ok.config().userAgent(), "aeronet-client");
  ok.clearIdleConnections();  // no-op on a fresh client, but exercises the accessor

  // Encoding::none simply means "request compression disabled": it constructs fine (no longer an error).
  {
    HttpClientConfig cfg;
    cfg.requestCompression.encoding = Encoding::none;
    EXPECT_NO_THROW(HttpClient{cfg});
  }

  // An out-of-range codec parameter is rejected via CompressionConfig::validate() once a codec is set.
  {
    HttpClientConfig cfg;
    cfg.withRequestCompression(Encoding::gzip);
    cfg.requestCompression.codec.maxCompressRatio = 2.0F;  // must be in (0, 1)
    EXPECT_THROW(HttpClient{cfg}, std::exception);
  }
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

}  // namespace aeronet
