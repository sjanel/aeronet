#include <gtest/gtest.h>

#include <stdexcept>

#include "aeronet/decompression-config.hpp"
#include "aeronet/http-server-config.hpp"

namespace aeronet {

TEST(ExtraConfigValidations, TlsMissingCertKey) {
  HttpServerConfig cfg;
  cfg.tls.enabled = true;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(ExtraConfigValidations, TlsAlpnMustMatch) {
  HttpServerConfig cfg;
  cfg.tls.enabled = true;
  cfg.tls.withCertPem("dummy");
  cfg.tls.withKeyPem("dummy");
  cfg.tls.alpnMustMatch = true;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(ExtraConfigValidations, DecompressionChecks) {
  HttpServerConfig cfg;
  DecompressionConfig decompressionConfig;
  decompressionConfig.decoderChunkSize = 0;
  cfg.withRequestDecompression(decompressionConfig);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
  decompressionConfig.decoderChunkSize = 1024;
  decompressionConfig.maxDecompressedBytes = 512;
  EXPECT_THROW(decompressionConfig.validate(), std::invalid_argument);
}

TEST(ExtraConfigValidations, HeaderBodyLimits) {
  HttpServerConfig cfg;
  cfg.maxHeaderBytes = 10;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
  cfg.maxHeaderBytes = 1024;
  cfg.maxBodyBytes = 0;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

#if defined(AERONET_ENABLE_OPENSSL) && defined(AERONET_ENABLE_KTLS)
TEST(ExtraConfigValidations, KtlsModeWithoutCredentialsThrows) {
  HttpServerConfig cfg;
  cfg.withTlsKtlsMode(TLSConfig::KtlsMode::Enabled);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(ExtraConfigValidations, KtlsRequiresCredentials) {
  HttpServerConfig cfg;
  cfg.withTlsKtlsMode(TLSConfig::KtlsMode::Auto);
  cfg.withTlsCertKeyMemory("cert", "key");
  EXPECT_NO_THROW(cfg.validate());
}
#endif

}  // namespace aeronet
