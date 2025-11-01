#include <gtest/gtest.h>

#include <stdexcept>

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
  cfg.decompression.decoderChunkSize = 0;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
  cfg.decompression.decoderChunkSize = 1024;
  cfg.decompression.maxDecompressedBytes = 512;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(ExtraConfigValidations, HeaderBodyLimits) {
  HttpServerConfig cfg;
  cfg.maxHeaderBytes = 10;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
  cfg.maxHeaderBytes = 1024;
  cfg.maxBodyBytes = 0;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

}  // namespace aeronet
