#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/aeronet.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client-exception.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/test-tls-helper.hpp"
#include "aeronet/tls-config.hpp"

namespace aeronet {
namespace {

// Write PEM text to a unique temp file and return its path (caller-owned for the test duration).
std::filesystem::path WriteTempPem(std::string_view pem, std::string_view tag) {
  static int counter = 0;
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("aeronet-client-test-" + std::string(tag) + "-" + std::to_string(++counter) + ".pem");
  std::ofstream(path) << pem;
  return path;
}

// HTTPS end-to-end: an aeronet TLS server with an ephemeral self-signed cert, hit by HttpClient.
class HttpClientTlsE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto [certPem, keyPem] = test::MakeEphemeralCertKey("localhost");
    _certPem = certPem;

    TLSConfig tls;
    tls.enabled = true;
    tls.withCertPem(certPem).withKeyPem(keyPem);

    Router router;
    router.setPath(http::Method::GET, "/secure",
                   [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "secret", "text/plain"); });

    HttpServerConfig cfg;
    // Keep-alive must stay comfortably longer than a TLS handshake can take on a loaded CI runner.
    // The keep-alive sweep reaps a connection whose deadline elapses, and a connection still completing
    // its handshake is not exempt (handshakeTimeout, the dedicated bound, is disabled by default). An
    // aggressive value here let the server starve and reap the legitimate handshake, surfacing as a flaky
    // "TLS handshake failed" on the client. Use the library default (5s) — these tests never assert on
    // idle-reaping, so there is no reason to tighten it.
    cfg.withPort(0).withKeepAliveTimeout(std::chrono::seconds{5}).withPollInterval(std::chrono::milliseconds{20});
    cfg.tls = std::move(tls);

    _server = std::make_unique<SingleHttpServer>(std::move(cfg), std::move(router));
    _port = _server->port();
    _server->start();
  }

  void TearDown() override { _server.reset(); }

  [[nodiscard]] std::string url(std::string_view path) const {
    return "https://localhost:" + std::to_string(_port) + std::string(path);
  }

  std::unique_ptr<SingleHttpServer> _server;
  std::string _certPem;
  uint16_t _port{0};
};

}  // namespace

TEST_F(HttpClientTlsE2ETest, GetOverTlsWithoutVerification) {
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;  // self-signed cert, not in any trust store
  HttpClient client(cfg);
  auto resp = client.get(url("/secure")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "secret");
}

TEST_F(HttpClientTlsE2ETest, VerificationFailsForUntrustedCert) {
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = true;  // default trust store does not contain our ephemeral CA
  HttpClient client(cfg);
  // The handshake completes the TCP connect but the peer certificate is rejected: a runtime TLS error.
  auto result = client.get(url("/secure"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::tlsError);
}

TEST_F(HttpClientTlsE2ETest, TrustsServerViaCaFile) {
  // Trust the server's self-signed cert by pointing the client at it as a CA file: exercises the
  // SSL_CTX_load_verify_locations (CA-file) trust-store branch with a successful verification.
  const auto caPath = WriteTempPem(_certPem, "ca");
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = true;
  cfg.withTlsCaFile(caPath.string());
  HttpClient client(cfg);
  auto resp = client.get(url("/secure")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "secret");
  std::filesystem::remove(caPath);
}

TEST_F(HttpClientTlsE2ETest, BadCaFileThrows) {
  // A non-existent CA file makes SSL_CTX_load_verify_locations fail at context build time.
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = true;
  cfg.withTlsCaFile("/nonexistent/aeronet-no-such-ca.pem");
  HttpClient client(cfg);
  EXPECT_THROW({ [[maybe_unused]] auto res = client.get(url("/secure")); }, HttpClientException);
}

TEST_F(HttpClientTlsE2ETest, KeepAliveOverTls) {
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  HttpClient client(cfg);
  for (int i = 0; i < 3; ++i) {
    auto resp = client.get(url("/secure")).value();
    EXPECT_EQ(resp.status(), 200);
    EXPECT_EQ(resp.bodyInMemory(), "secret");
  }
}

TEST_F(HttpClientTlsE2ETest, HonoursCipherListAndVersionBounds) {
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  // Exercises the cipher-list + min/max protocol-version branches of the client TLS context setup.
  cfg.withTlsCipherList("HIGH").withTlsMinVersion(TLSConfig::TLS_1_2).withTlsMaxVersion(TLSConfig::TLS_1_3);
  HttpClient client(cfg);
  auto resp = client.get(url("/secure")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "secret");
}

TEST_F(HttpClientTlsE2ETest, UnsetMinVersionUsesLibraryDefault) {
  // An unset (default-constructed) min version maps to OpenSSL constant 0, so the client leaves the
  // library default floor in place instead of pinning a minimum (exercises ToOpenSslTlsVersion's 0 path).
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.tlsMinVersion = TLSConfig::Version{};
  HttpClient client(cfg);
  auto resp = client.get(url("/secure")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "secret");
}

// --- TLS context build failures (all surface as HttpClientException at first-request time) ---

TEST(HttpClientTlsErrorTest, InvalidMaxVersionThrows) {
  // TLS 1.1 is neither of the two supported constants, so ToOpenSslTlsVersion returns 0 for a non-unset
  // max version -> the context build rejects it.
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withTlsMaxVersion(TLSConfig::Version{1, 1});
  HttpClient client(cfg);
  EXPECT_THROW({ [[maybe_unused]] auto res = client.get("https://localhost:9/"); }, HttpClientException);
}

TEST(HttpClientTlsErrorTest, InvalidCipherListThrows) {
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withTlsCipherList("THIS-IS-NOT-A-REAL-CIPHER");  // matches no cipher -> SSL_CTX_set_cipher_list fails
  HttpClient client(cfg);
  EXPECT_THROW({ [[maybe_unused]] auto res = client.get("https://localhost:9/"); }, HttpClientException);
}

TEST(HttpClientTlsErrorTest, GarbageInMemoryClientCertThrows) {
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withTlsClientCertKeyMemory("not-a-pem-cert", "not-a-pem-key");  // PEM parse fails
  HttpClient client(cfg);
  EXPECT_THROW({ [[maybe_unused]] auto res = client.get("https://localhost:9/"); }, HttpClientException);
}

TEST(HttpClientTlsErrorTest, MismatchedClientCertKeyThrows) {
  // A valid certificate paired with a valid but unrelated private key: parse + install succeed, then
  // SSL_CTX_check_private_key rejects the pair.
  auto [certA, keyA] = test::MakeEphemeralCertKey("client-a");
  auto [certB, keyB] = test::MakeEphemeralCertKey("client-b");
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withTlsClientCertKeyMemory(certA, keyB);  // cert of A, key of B
  HttpClient client(cfg);
  EXPECT_THROW({ [[maybe_unused]] auto res = client.get("https://localhost:9/"); }, HttpClientException);
}

TEST(HttpClientTlsErrorTest, MissingClientCertFileThrows) {
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withTlsClientCertKeyFile("/nonexistent/aeronet-no-cert.pem", "/nonexistent/aeronet-no-key.pem");
  HttpClient client(cfg);
  EXPECT_THROW({ [[maybe_unused]] auto res = client.get("https://localhost:9/"); }, HttpClientException);
}

TEST(HttpClientTlsErrorTest, ValidCertFileWithBadKeyFileThrows) {
  // A real certificate file but a missing key file: the certificate loads, then the key load fails.
  auto [cert, key] = test::MakeEphemeralCertKey("localhost");
  const auto certPath = WriteTempPem(cert, "okcert");
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withTlsClientCertKeyFile(certPath.string(), "/nonexistent/aeronet-no-key.pem");
  HttpClient client(cfg);
  EXPECT_THROW({ [[maybe_unused]] auto res = client.get("https://localhost:9/"); }, HttpClientException);
  std::filesystem::remove(certPath);
}

// Mutual TLS: the server requires (and trusts) a client certificate; the client presents one.
namespace {
std::unique_ptr<SingleHttpServer> MakeMtlsServer(std::string_view serverCert, std::string_view serverKey,
                                                 std::string_view trustedClientCert, uint16_t& port) {
  Router router;
  router.setPath(http::Method::GET, "/secure",
                 [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "secret", "text/plain"); });

  // Configure everything through the cfg builders: withTlsCertKeyMemory / withTlsRequireClientCert all
  // operate on cfg.tls in place. Assigning cfg.tls afterwards would wipe the mTLS settings.
  HttpServerConfig cfg;
  cfg.withPort(0)
      .withPollInterval(std::chrono::milliseconds{20})
      .withTlsCertKeyMemory(serverCert, serverKey)
      .withTlsRequireClientCert(true)
      .withTlsTrustedClientCert(trustedClientCert);

  auto server = std::make_unique<SingleHttpServer>(std::move(cfg), std::move(router));
  port = server->port();
  server->start();
  return server;
}
}  // namespace

TEST(HttpClientMtlsTest, MutualTlsRoundTrip) {
  auto [serverCert, serverKey] = test::MakeEphemeralCertKey("localhost");
  auto [clientCert, clientKey] = test::MakeEphemeralCertKey("test-client");
  uint16_t port{0};
  auto server = MakeMtlsServer(serverCert, serverKey, clientCert, port);
  const std::string url = "https://localhost:" + std::to_string(port) + "/secure";

  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;  // self-signed server cert
  for (std::string_view clientCertSv : {std::string_view(), std::string_view(clientCert)}) {
    for (std::string_view clientKeySv : {std::string_view(), std::string_view(clientKey)}) {
      cfg.withTlsClientCertKeyMemory(clientCertSv, clientKeySv);

      HttpClient client(cfg);
      auto httpClientResult = client.get(url);
      if (clientCertSv.empty() || clientKeySv.empty()) {
        EXPECT_FALSE(httpClientResult);
      } else {
        const auto& resp = httpClientResult.value();
        EXPECT_EQ(resp.status(), 200);
        EXPECT_EQ(resp.bodyInMemory(), "secret");
      }
    }
  }
}

TEST(HttpClientMtlsTest, MutualTlsRoundTripFromFiles) {
  auto [serverCert, serverKey] = test::MakeEphemeralCertKey("localhost");
  auto [clientCert, clientKey] = test::MakeEphemeralCertKey("test-client");
  uint16_t port{0};
  auto server = MakeMtlsServer(serverCert, serverKey, clientCert, port);
  const std::string url = "https://localhost:" + std::to_string(port) + "/secure";

  // Present the client certificate from PEM files (exercises the file-based mTLS load path).
  const auto certPath = WriteTempPem(clientCert, "clicert");
  const auto keyPath = WriteTempPem(clientKey, "clikey");

  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withTlsClientCertKeyFile(certPath.string(), keyPath.string());
  HttpClient client(cfg);
  auto resp = client.get(url).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "secret");

  std::filesystem::remove(certPath);
  std::filesystem::remove(keyPath);
}

// A server that requires a client certificate must reject the TLS handshake when the client presents
// none. This is the server-side mTLS enforcement guarantee (regression test for the case where the
// server requested but did not actually require, letting certificate-less clients through).
TEST(HttpClientMtlsTest, RejectsClientWithoutCert) {
  auto [serverCert, serverKey] = test::MakeEphemeralCertKey("localhost");
  auto [clientCert, clientKey] = test::MakeEphemeralCertKey("test-client");
  uint16_t port{0};
  auto server = MakeMtlsServer(serverCert, serverKey, clientCert, port);
  const std::string url = "https://localhost:" + std::to_string(port) + "/secure";

  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;  // self-signed server cert; the client presents NO certificate
  HttpClient client(cfg);
  // The server aborts the connection because no client certificate is presented. With TLS 1.3 the client
  // can finish its half of the handshake and start sending before the server's alert arrives, so the
  // failure surfaces non-deterministically as a TLS handshake, write or read error -- assert only that the
  // request did not succeed (it never yields a response).
  auto result = client.get(url);
  ASSERT_FALSE(result);
}

}  // namespace aeronet
