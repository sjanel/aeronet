#include <gtest/gtest.h>

#include "aeronet/http-server-config.hpp"
#include "invalid_argument_exception.hpp"

namespace aeronet {

TEST(ExtraConfigValidations, TlsMissingCertKey) {
  HttpServerConfig cfg;
  cfg.tls.enabled = true;
  EXPECT_THROW(cfg.validate(), invalid_argument);
}

TEST(ExtraConfigValidations, TlsAlpnMustMatch) {
  HttpServerConfig cfg;
  cfg.tls.enabled = true;
  cfg.tls.withCertPem("dummy");
  cfg.tls.withKeyPem("dummy");
  cfg.tls.alpnMustMatch = true;
  EXPECT_THROW(cfg.validate(), invalid_argument);
}

TEST(ExtraConfigValidations, DecompressionChecks) {
  HttpServerConfig cfg;
  cfg.requestDecompression.decoderChunkSize = 0;
  EXPECT_THROW(cfg.validate(), invalid_argument);
  cfg.requestDecompression.decoderChunkSize = 1024;
  cfg.requestDecompression.maxDecompressedBytes = 512;
  EXPECT_THROW(cfg.validate(), invalid_argument);
}

TEST(ExtraConfigValidations, HeaderBodyLimits) {
  HttpServerConfig cfg;
  cfg.maxHeaderBytes = 10;
  EXPECT_THROW(cfg.validate(), invalid_argument);
  cfg.maxHeaderBytes = 1024;
  cfg.maxBodyBytes = 0;
  EXPECT_THROW(cfg.validate(), invalid_argument);
}

}  // namespace aeronet
