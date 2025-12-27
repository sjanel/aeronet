#include "aeronet/tls-config.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace aeronet {

namespace {

constexpr std::string_view kDummyCertPem = "-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n";
constexpr std::string_view kDummyKeyPem = "-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n";

}  // namespace

TEST(HttpTlsVersionBounds, ValidMinVersion) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withTlsMinVersion("TLS1.2");
  cfg.withKeyPem(kDummyKeyPem);
  cfg.withCertPem(kDummyCertPem);
  EXPECT_NO_THROW(cfg.validate());
}

TEST(HttpTlsVersionBounds, InvalidMinVersionThrows) {
  // Provide unsupported version token -> validate() should throw
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withTlsMinVersion("TLS1.1");
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TlsConfigTest, InvalidMinVersionThrows) {
  // Covers tls-config.cpp lines 59-60
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("DUMMY").withKeyPem("DUMMY");
  cfg.minVersion = {1, 0};  // TLS 1.0 is not supported

  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TlsConfigTest, InvalidMaxVersionThrows) {
  // Covers tls-config.cpp lines 65-66
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("DUMMY").withKeyPem("DUMMY");
  cfg.maxVersion = {1, 1};  // TLS 1.1 is not supported

  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TLSConfigTest, SessionTicketKeysConfigured) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withKeyPem(kDummyKeyPem);
  cfg.withCertPem(kDummyCertPem);
  cfg.withTlsSessionTicketKey(TLSConfig::SessionTicketKey{});
  cfg.withTlsSessionTickets();
  EXPECT_NO_THROW(cfg.validate());

  EXPECT_TRUE(cfg.sniCertificates().empty());
  EXPECT_EQ(cfg.certFile(), std::string_view());
  EXPECT_EQ(cfg.keyFile(), std::string_view());
}

TEST(TLSConfigTest, SessionTicketKeysConfiguredButTicketsDisabledThrows) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withKeyPem(kDummyKeyPem);
  cfg.withCertPem(kDummyCertPem);
  cfg.withTlsSessionTicketKey(TLSConfig::SessionTicketKey{});
  cfg.withTlsSessionTickets(false);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TLSConfigTest, HandshakeRateLimitBurstWithRate) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withKeyPem(kDummyKeyPem);
  cfg.withCertPem(kDummyCertPem);
  cfg.handshakeRateLimitPerSecond = 1;
  cfg.handshakeRateLimitBurst = 10;
  cfg.validate();
}

TEST(TLSConfigTest, HandshakeRateLimitBurstWithoutRateThrows) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withKeyPem(kDummyKeyPem);
  cfg.withCertPem(kDummyCertPem);
  cfg.handshakeRateLimitPerSecond = 0;
  cfg.handshakeRateLimitBurst = 10;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TLSConfigTest, SniCertificatePatternNonEmpty) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withKeyPem(kDummyKeyPem);
  cfg.withCertPem(kDummyCertPem);
  EXPECT_THROW(cfg.withTlsSniCertificateMemory("", kDummyCertPem, kDummyKeyPem), std::invalid_argument);
}

TEST(TLSConfigTest, InvalidWildcard) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withKeyPem(kDummyKeyPem);
  cfg.withCertPem(kDummyCertPem);
  EXPECT_THROW(cfg.withTlsSniCertificateMemory("*.", kDummyCertPem, kDummyKeyPem), std::invalid_argument);
}

TEST(TLSConfigTest, SessionTicketsConfigEquality) {
  TLSConfig::SessionTicketsConfig cfg1;
  cfg1.enabled = true;
  cfg1.lifetime = std::chrono::seconds(7200);
  cfg1.maxKeys = 5;

  TLSConfig cfg;

  cfg.withTlsSessionTickets(true);
  cfg.withTlsSessionTicketLifetime(std::chrono::seconds(7200));
  cfg.withTlsSessionTicketMaxKeys(5);

  EXPECT_EQ(cfg1, cfg.sessionTickets);

  cfg.withTlsSessionTicketMaxKeys(10);
  EXPECT_NE(cfg1, cfg.sessionTickets);
}

TEST(TLSConfigTest, SniCertificateEquality) {
  TLSConfig::SniCertificate cert1;
  cert1.setPattern("example.com");
  cert1.isWildcard = false;
  cert1.setCertPem(kDummyCertPem);
  cert1.setKeyPem(kDummyKeyPem);
  TLSConfig::SniCertificate cert2;
  cert2.setPattern("example.com");
  cert2.isWildcard = false;
  cert2.setCertPem(kDummyCertPem);
  cert2.setKeyPem(kDummyKeyPem);
  EXPECT_EQ(cert1, cert2);

  cert2.setKeyPem("-----BEGIN PRIVATE KEY-----\nDIFFERENT\n-----END PRIVATE KEY-----\n");
  EXPECT_NE(cert1, cert2);
}

TEST(TLSConfigTest, RequiresCertAndKeyWhenEnabled) {
  TLSConfig cfg;
  cfg.enabled = true;
  // neither cert nor key -> error
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  // only cert provided
  cfg.withCertPem(kDummyCertPem);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  // only key provided
  TLSConfig cfg2;
  cfg2.enabled = true;
  cfg2.withKeyPem(kDummyKeyPem);
  EXPECT_THROW(cfg2.validate(), std::invalid_argument);

  // both present -> ok
  TLSConfig cfg3;
  cfg3.enabled = true;
  cfg3.withCertPem(kDummyCertPem);
  cfg3.withKeyPem(kDummyKeyPem);
  EXPECT_NO_THROW(cfg3.validate());
}

TEST(TLSConfigTest, RequireClientCertNeedsTrustedCerts) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(kDummyCertPem);
  cfg.withKeyPem(kDummyKeyPem);

  cfg.requireClientCert = true;
  // no trusted client certs -> validation fails
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  cfg.withTlsTrustedClientCert("-----BEGIN CERTIFICATE-----\nFAKECLIENT\n-----END CERTIFICATE-----\n");
  EXPECT_NO_THROW(cfg.validate());
}

TEST(TLSConfigTest, DisableCompression) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(kDummyCertPem).withKeyPem(kDummyKeyPem);
  cfg.withTlsDisableCompression();

  EXPECT_NO_THROW(cfg.validate());

  EXPECT_TRUE(cfg.disableCompression);
}

TEST(TLSConfigTest, AlpnMustMatchRequiresProtocols) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(kDummyCertPem);
  cfg.withKeyPem(kDummyKeyPem);

  cfg.alpnMustMatch = true;
  // no protocols configured
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  cfg.withTlsAlpnProtocols(std::initializer_list<std::string_view>{"http/1.1"});
  EXPECT_NO_THROW(cfg.validate());

  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  EXPECT_STREQ(cfg.certPemCstr(), kDummyCertPem.data());
  EXPECT_STREQ(cfg.keyPemCstr(), kDummyKeyPem.data());  // NOLINT(bugprone-suspicious-stringview-data-usage)
}

TEST(TLSConfigTest, AlpnProtocolEntriesNonEmptyAndWithinLimit) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(kDummyCertPem);
  cfg.withKeyPem(kDummyKeyPem);

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

TEST(TLSConfigTest, MinMaxVersionValidation) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(kDummyCertPem);
  cfg.withKeyPem(kDummyKeyPem);

  cfg.withTlsMinVersion("TLS1.2");
  cfg.withTlsMaxVersion("TLS1.3");
  EXPECT_NO_THROW(cfg.validate());

  // unsupported value already tested elsewhere; test mismatched ordering allowed (validate only checks tokens)
  cfg.withTlsMinVersion("TLS1.3");
  cfg.withTlsMaxVersion("TLS1.2");
  EXPECT_NO_THROW(cfg.validate());
}

TEST(TLSConfigTest, KtlsModeBuildGuard) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(kDummyCertPem);
  cfg.withKeyPem(kDummyKeyPem);

  cfg.withKtlsMode(TLSConfig::KtlsMode::Opportunistic);
  EXPECT_NO_THROW(cfg.validate());

  cfg.withKtlsMode(TLSConfig::KtlsMode::Disabled);
  EXPECT_NO_THROW(cfg.validate());

  cfg.withKtlsMode(TLSConfig::KtlsMode::Enabled);

  EXPECT_NO_THROW(cfg.validate());

  cfg.withKtlsMode(TLSConfig::KtlsMode::Required);

  EXPECT_NO_THROW(cfg.validate());

  cfg.withKtlsMode(
      static_cast<TLSConfig::KtlsMode>(std::numeric_limits<std::underlying_type_t<TLSConfig::KtlsMode>>::max()));
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TlsConfigTest, ClearSniCertificatesWorks) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem(kDummyCertPem).withKeyPem(kDummyKeyPem);
  cfg.withTlsSniCertificateMemory("sub.example.com", kDummyCertPem, kDummyKeyPem);

  EXPECT_EQ(cfg.sniCertificates().size(), 1U);
  const auto& cert = cfg.sniCertificates().front();
  EXPECT_EQ(cert.pattern(), "sub.example.com");

  cfg.clearTlsSniCertificates();
  EXPECT_TRUE(cfg.sniCertificates().empty());
}

TEST(TlsConfigTest, SessionTicketsMaxKeysZeroThrows) {
  TLSConfig cfg;
  cfg.enabled = true;
  cfg.withCertPem("DUMMY").withKeyPem("DUMMY");
  cfg.sessionTickets.maxKeys = 0;

  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TlsConfigTest, EmptySniHostnameMemoryThrows) {
  TLSConfig cfg;
  cfg.enabled = true;

  EXPECT_THROW(cfg.withTlsSniCertificateMemory("", "cert", "key"), std::invalid_argument);
}

TEST(TlsConfigTest, EmptySniHostnameFilesThrows) {
  TLSConfig cfg;
  cfg.enabled = true;

  EXPECT_THROW(cfg.withTlsSniCertificateFiles("", "/path/cert", "/path/key"), std::invalid_argument);
}

TEST(TlsConfigTest, EmptySniCertPemThrows) {
  TLSConfig cfg;
  cfg.enabled = true;

  EXPECT_THROW(cfg.withTlsSniCertificateMemory("example.com", "", "key"), std::invalid_argument);
  EXPECT_THROW(cfg.withTlsSniCertificateMemory("example.com", "cert", ""), std::invalid_argument);
}

TEST(TlsConfigTest, EmptySniCertFileThrows) {
  TLSConfig cfg;
  cfg.enabled = true;

  EXPECT_THROW(cfg.withTlsSniCertificateFiles("example.com", "", "/path/key"), std::invalid_argument);
  EXPECT_THROW(cfg.withTlsSniCertificateFiles("example.com", "/path/cert", ""), std::invalid_argument);
}

TEST(TlsConfigSniCertificateTest, HasFilesAndHasPemCombinations) {
  TLSConfig::SniCertificate cert;
  // All empty
  EXPECT_TRUE(cert.certFile().empty());
  EXPECT_TRUE(cert.keyFile().empty());
  EXPECT_TRUE(cert.certPem().empty());
  EXPECT_TRUE(cert.keyPem().empty());
  EXPECT_FALSE(cert.hasFiles());
  EXPECT_FALSE(cert.hasPem());

  // Cert file only
  cert.setCertFile("/etc/ssl/cert.pem");
  EXPECT_TRUE(cert.hasFiles());
  EXPECT_FALSE(cert.hasPem());

  // Reset, Key file only
  cert.setCertFile("");
  cert.setKeyFile("/etc/ssl/key.pem");
  EXPECT_TRUE(cert.hasFiles());
  EXPECT_FALSE(cert.hasPem());

  // Both files
  cert.setCertFile("/etc/ssl/cert.pem");
  EXPECT_TRUE(cert.hasFiles());

  // Clear files, set PEM cert only
  cert.setCertFile("");
  cert.setKeyFile("");
  cert.setCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  EXPECT_FALSE(cert.hasFiles());
  EXPECT_TRUE(cert.hasPem());

  // PEM key only
  cert.setCertPem("");
  cert.setKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  EXPECT_FALSE(cert.hasFiles());
  EXPECT_TRUE(cert.hasPem());

  // Both PEMs
  cert.setCertPem("-----BEGIN CERTIFICATE-----\nFAKE\n-----END CERTIFICATE-----\n");
  EXPECT_TRUE(cert.hasPem());

  // Mixed: file for cert, PEM for key -> both hasFiles and hasPem true/false appropriately
  cert.setCertPem("");
  cert.setKeyPem("-----BEGIN PRIVATE KEY-----\nFAKE\n-----END PRIVATE KEY-----\n");
  cert.setCertFile("/etc/ssl/cert.pem");
  EXPECT_TRUE(cert.hasFiles());
  EXPECT_TRUE(cert.hasPem());
}

TEST(TlsConfigTest, ClearTlsSessionTicketKeys) {
  TLSConfig cfg;
  cfg.enabled = true;

  TLSConfig::SessionTicketKey key{};
  key.fill(std::byte{0x1});

  cfg.withTlsSessionTicketKey(key);
  cfg.withTlsSessionTicketKey(key);

  EXPECT_EQ(cfg.sessionTicketKeys().size(), 2U);

  cfg.clearTlsSessionTicketKeys();
  EXPECT_TRUE(cfg.sessionTicketKeys().empty());
}

TEST(TlsConfigTest, WithTlsHandshakeConcurrencyLimitSetsValue) {
  TLSConfig cfg;
  cfg.enabled = true;

  cfg.withTlsHandshakeConcurrencyLimit(5);
  EXPECT_EQ(cfg.maxConcurrentHandshakes, 5U);

  cfg.withTlsHandshakeConcurrencyLimit(0);
  EXPECT_EQ(cfg.maxConcurrentHandshakes, 0U);
}

TEST(TlsConfigTest, WithTlsHandshakeRateLimitSetsValues) {
  TLSConfig cfg;
  cfg.enabled = true;

  cfg.withTlsHandshakeRateLimit(20, 100);
  EXPECT_EQ(cfg.handshakeRateLimitPerSecond, 20U);
  EXPECT_EQ(cfg.handshakeRateLimitBurst, 100U);

  // Changing values should overwrite previous ones
  cfg.withTlsHandshakeRateLimit(0, 0);
  EXPECT_EQ(cfg.handshakeRateLimitPerSecond, 0U);
  EXPECT_EQ(cfg.handshakeRateLimitBurst, 0U);
}

TEST(TlsConfigTest, WithoutTlsTrustedClientCertClearsList) {
  TLSConfig cfg;
  cfg.enabled = true;

  cfg.withTlsTrustedClientCert("-----BEGIN CERTIFICATE-----\nCLIENT1\n-----END CERTIFICATE-----\n");
  cfg.withTlsTrustedClientCert("-----BEGIN CERTIFICATE-----\nCLIENT2\n-----END CERTIFICATE-----\n");

  auto before = cfg.trustedClientCertsPem();
  EXPECT_GT(std::distance(before.begin(), before.end()), 0);

  cfg.withoutTlsTrustedClientCert();
  auto after = cfg.trustedClientCertsPem();
  EXPECT_EQ(std::distance(after.begin(), after.end()), 0);
}

}  // namespace aeronet