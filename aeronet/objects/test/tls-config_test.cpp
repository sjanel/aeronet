#include "aeronet/tls-config.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

TEST(HttpTlsVersionBounds, InvalidMinVersionThrows) {
  // Provide unsupported version token -> validate() should throw
  aeronet::TLSConfig cfg;
  cfg.enabled = true;
  cfg.withTlsMinVersion("TLS1.1");
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpTlsVersionBounds, ValidMinVersion) {
  aeronet::TLSConfig cfg;
  cfg.enabled = true;
  cfg.withTlsMinVersion("TLS1.2");
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  EXPECT_NO_THROW(cfg.validate());
}

TEST(TLSConfigValidate, RequiresCertAndKeyWhenEnabled) {
  aeronet::TLSConfig cfg;
  cfg.enabled = true;
  // neither cert nor key -> error
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  // only cert provided
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  // only key provided
  aeronet::TLSConfig cfg2;
  cfg2.enabled = true;
  cfg2.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  EXPECT_THROW(cfg2.validate(), std::invalid_argument);

  // both present -> ok
  aeronet::TLSConfig cfg3;
  cfg3.enabled = true;
  cfg3.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cfg3.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  EXPECT_NO_THROW(cfg3.validate());
}

TEST(TLSConfigValidate, RequireClientCertNeedsTrustedCerts) {
  aeronet::TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");

  cfg.requireClientCert = true;
  // no trusted client certs -> validation fails
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  cfg.withTlsTrustedClientCert("-----BEGIN CERTIFICATE-----\nFAKECLIENT\n-----END CERTIFICATE-----\n");
  EXPECT_NO_THROW(cfg.validate());
}

TEST(TLSConfigValidate, AlpnMustMatchRequiresProtocols) {
  aeronet::TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");

  cfg.alpnMustMatch = true;
  // no protocols configured
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  cfg.withTlsAlpnProtocols(std::initializer_list<std::string_view>{"http/1.1"});
  EXPECT_NO_THROW(cfg.validate());
}

TEST(TLSConfigValidate, AlpnProtocolEntriesNonEmptyAndWithinLimit) {
  aeronet::TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");

  // empty entry -> invalid
  cfg.withTlsAlpnProtocols(std::initializer_list<std::string_view>{"http/1.1", ""});
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  // too-long entry -> invalid
  std::string longProto(aeronet::TLSConfig::kMaxAlpnProtocolLength + 1, 'x');
  cfg.withTlsAlpnProtocols(std::initializer_list<std::string_view>{longProto});
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  // valid short entries -> ok
  cfg.withTlsAlpnProtocols(std::initializer_list<std::string_view>{"http/1.1", "h2"});
  EXPECT_NO_THROW(cfg.validate());
}

TEST(TLSConfigValidate, MinMaxVersionValidation) {
  aeronet::TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");

  cfg.withTlsMinVersion("TLS1.2");
  cfg.withTlsMaxVersion("TLS1.3");
  EXPECT_NO_THROW(cfg.validate());

  // unsupported value already tested elsewhere; test mismatched ordering allowed (validate only checks tokens)
  cfg.withTlsMinVersion("TLS1.3");
  cfg.withTlsMaxVersion("TLS1.2");
  EXPECT_NO_THROW(cfg.validate());
}

TEST(TLSConfigValidate, KtlsModeBuildGuard) {
  aeronet::TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");

#ifndef AERONET_ENABLE_KTLS
  cfg.withKtlsMode(aeronet::TLSConfig::KtlsMode::Auto);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
#else
  cfg.withKtlsMode(aeronet::TLSConfig::KtlsMode::Auto);
  EXPECT_NO_THROW(cfg.validate());
#endif
}