#include "aeronet/tls-config.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aeronet {

TEST(HttpTlsVersionBounds, InvalidMinVersionThrows) {
  // Provide unsupported version token -> validate() should throw
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withTlsMinVersion("TLS1.1");
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpTlsVersionBounds, ValidMinVersion) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withTlsMinVersion("TLS1.2");
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  EXPECT_NO_THROW(cfg.validate());
}

TEST(TLSConfigValidate, SessionTicketKeysConfiguredButTicketsDisabledThrows) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cfg.withTlsSessionTicketKey(TLSConfig::SessionTicketKey{});
  cfg.sessionTickets.enabled = false;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TLSConfigValidate, HandshakeRateLimitBurstWithoutRateThrows) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cfg.handshakeRateLimitPerSecond = 0;
  cfg.handshakeRateLimitBurst = 10;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TLSConfigValidate, SniCertificatePatternNonEmpty) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  EXPECT_THROW(cfg.withTlsSniCertificateMemory("", "-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n",
                                               "-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n"),
               std::invalid_argument);
}

TEST(TLSConfigValidate, InvalidWildcard) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  EXPECT_THROW(cfg.withTlsSniCertificateMemory("*.", "-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n",
                                               "-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n"),
               std::invalid_argument);
}

TEST(TLSConfig, SessionTicketsConfigEquality) {
  TLSConfig::SessionTicketsConfig cfg1;
  cfg1.enabled = true;
  cfg1.lifetime = std::chrono::seconds(7200);
  cfg1.maxKeys = 5;

  TLSConfig::SessionTicketsConfig cfg2;
  cfg2.enabled = true;
  cfg2.lifetime = std::chrono::seconds(7200);
  cfg2.maxKeys = 5;

  EXPECT_EQ(cfg1, cfg2);

  cfg2.maxKeys = 10;
  EXPECT_NE(cfg1, cfg2);
}

TEST(TLSConfig, SniCertificateEquality) {
  TLSConfig::SniCertificate cert1;
  cert1.setPattern("example.com");
  cert1.isWildcard = false;
  cert1.setCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cert1.setKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  TLSConfig::SniCertificate cert2;
  cert2.setPattern("example.com");
  cert2.isWildcard = false;
  cert2.setCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cert2.setKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  EXPECT_EQ(cert1, cert2);

  cert2.setKeyPem("-----BEGIN PRIVATE KEY-----\nDIFFERENT\n-----END PRIVATE KEY-----\n");
  EXPECT_NE(cert1, cert2);
}

TEST(TLSConfigValidate, RequiresCertAndKeyWhenEnabled) {
  TLSConfig cfg;
  cfg.enabled = true;
  // neither cert nor key -> error
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  // only cert provided
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  // only key provided
  TLSConfig cfg2;
  cfg2.enabled = true;
  cfg2.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  EXPECT_THROW(cfg2.validate(), std::invalid_argument);

  // both present -> ok
  TLSConfig cfg3;
  cfg3.enabled = true;
  cfg3.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cfg3.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  EXPECT_NO_THROW(cfg3.validate());
}

TEST(TLSConfigValidate, RequireClientCertNeedsTrustedCerts) {
  TLSConfig cfg;
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
  TLSConfig cfg;
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
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");

  // empty entry -> invalid
  cfg.withTlsAlpnProtocols(std::initializer_list<std::string_view>{"http/1.1", ""});
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  // too-long entry -> invalid
  std::string longProto(TLSConfig::kMaxAlpnProtocolLength + 1, 'x');
  cfg.withTlsAlpnProtocols(std::initializer_list<std::string_view>{longProto});
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  // valid short entries -> ok
  cfg.withTlsAlpnProtocols(std::initializer_list<std::string_view>{"http/1.1", "h2"});
  EXPECT_NO_THROW(cfg.validate());
}

TEST(TLSConfigValidate, MinMaxVersionValidation) {
  TLSConfig cfg;
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
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  cfg.withKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");

#ifndef AERONET_ENABLE_KTLS
  cfg.withKtlsMode(TLSConfig::KtlsMode::Auto);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
#else
  cfg.withKtlsMode(TLSConfig::KtlsMode::Auto);
  EXPECT_NO_THROW(cfg.validate());
#endif
}

}  // namespace aeronet