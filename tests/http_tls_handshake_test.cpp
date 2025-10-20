#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_temp_file.hpp"
#include "aeronet/test_tls_client.hpp"
#include "aeronet/test_tls_helper.hpp"
#include "aeronet/test_util.hpp"
#include "invalid_argument_exception.hpp"

using namespace aeronet;
using namespace std::chrono_literals;

TEST(HttpTlsBasic, HandshakeAndSimpleGet) {
  // Prepare config with in-memory self-signed cert/key
  aeronet::test::TlsTestServer ts;  // ephemeral TLS server
  ts.setDefault([](const aeronet::HttpRequest& req) {
    return aeronet::HttpResponse(200, "OK")
        .contentType(aeronet::http::ContentTypeTextPlain)
        .body(std::string("TLS OK ") + std::string(req.path()));
  });
  aeronet::test::TlsClient client(ts.port());
  auto raw = client.get("/hello", {{"X-Test", "tls"}});
  ASSERT_FALSE(raw.empty());
  ASSERT_TRUE(raw.contains("HTTP/1.1 200"));
  ASSERT_TRUE(raw.contains("TLS OK /hello"));
}

TEST(HttpTlsAlpnMismatch, HandshakeFailsWhenNoCommonProtocolAndMustMatch) {
  bool failed = false;
  aeronet::ServerStats statsAfter{};
  {
    aeronet::test::TlsTestServer ts({"http/1.1", "h2"},
                                    [](aeronet::HttpServerConfig& cfg) { cfg.withTlsAlpnMustMatch(true); });
    auto port = ts.port();
    ts.setDefault([](const aeronet::HttpRequest& req) {
      return aeronet::HttpResponse(aeronet::http::StatusCodeOK)
          .reason("OK")
          .contentType(aeronet::http::ContentTypeTextPlain)
          .body(std::string("ALPN:") + std::string(req.alpnProtocol()));
    });
    // Offer only a mismatching ALPN; since TlsClient uses options, construct with protoX.
    aeronet::test::TlsClient::Options opts;
    opts.alpn = {"protoX"};
    aeronet::test::TlsClient client(port, opts);
    failed = !client.handshakeOk();
    statsAfter = ts.stats();
    ts.stop();
  }
  ASSERT_TRUE(failed);
  ASSERT_GE(statsAfter.tlsAlpnStrictMismatches, 1U);
}

TEST(HttpTlsAlpnNonStrict, MismatchAllowedAndNoMetricIncrement) {
  std::string capturedAlpn;
  aeronet::ServerStats statsAfter{};
  {
    // Server prefers h2, but does NOT enforce match.
    aeronet::test::TlsTestServer ts({"h2"});
    auto port = ts.port();
    ts.setDefault([&](const aeronet::HttpRequest& req) {
      if (!req.alpnProtocol().empty()) {
        capturedAlpn = std::string(req.alpnProtocol());
      } else {
        capturedAlpn.clear();
      }
      return aeronet::HttpResponse(200, "OK").contentType(aeronet::http::ContentTypeTextPlain).body("NS");
    });
    aeronet::test::TlsClient::Options opts;
    opts.alpn = {"foo"};  // no overlap
    aeronet::test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto resp = client.get("/non_strict");
    statsAfter = ts.stats();
    ts.stop();
    ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  }
  // ALPN not negotiated => empty string
  ASSERT_TRUE(capturedAlpn.empty());
  ASSERT_EQ(statsAfter.tlsAlpnStrictMismatches, 0U);
  // Distribution should NOT contain client-only protocol
  for (const auto& kv : statsAfter.tlsAlpnDistribution) {
    ASSERT_NE(kv.first, std::string("foo"));
  }
}

// Verifies that moving a TLS+ALPN configured HttpServer prior to running preserves
// a valid TLS context and ALPN callback pointer. This specifically guards against
// the prior design where TlsContext was stored by value (e.g. inside std::optional):
// a move of HttpServer could relocate the TlsContext object while the OpenSSL
// SSL_CTX ALPN selection callback still held the old address -> use-after-free /
// crash during handshake. The current design stores TlsContext behind a stable
// std::unique_ptr, so the address observed by OpenSSL remains valid after moves.
//
// This test would (non-deterministically) fail or ASan-crash under the old design
// when compiled with sanitizers and run enough times, especially under load, but
// here we simply assert successful handshake + ALPN negotiation after a move.

TEST(HttpTlsMoveAlpn, MoveConstructBeforeRunMaintainsAlpnHandshake) {
  auto pair = aeronet::test::makeEphemeralCertKey();
  ASSERT_FALSE(pair.first.empty());
  ASSERT_FALSE(pair.second.empty());

  aeronet::HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(pair.first, pair.second);
  cfg.withTlsAlpnProtocols({"h2", "http/1.1"});  // offer both; client will request http/1.1 only

  aeronet::HttpServer original(cfg);
  original.router().setDefault([](const aeronet::HttpRequest& req) {
    return aeronet::HttpResponse(200, "OK")
        .contentType(aeronet::http::ContentTypeTextPlain)
        .body(std::string("MOVEALPN:") + (req.alpnProtocol().empty() ? "-" : std::string(req.alpnProtocol())));
  });

  auto port = original.port();
  aeronet::HttpServer moved(std::move(original));

  std::atomic_bool stop{false};
  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });

  // Actively wait until the listening socket accepts a plain TCP connection to avoid race.
  // This replicates TestServer readiness logic without duplicating its wrapper.
  {
    aeronet::test::ClientConnection probe(port, std::chrono::milliseconds{500});
  }

  aeronet::test::TlsClient::Options opts;
  opts.alpn = {"http/1.1"};
  aeronet::test::TlsClient client(port, opts);
  ASSERT_TRUE(client.handshakeOk()) << "TLS handshake failed after move (potential stale TlsContext pointer)";
  auto raw = client.get("/moved");
  stop.store(true);

  ASSERT_TRUE(raw.contains("HTTP/1.1 200"));
  ASSERT_TRUE(raw.contains("MOVEALPN:http/1.1")) << raw;
}

// Test mutual TLS requirement and ALPN negotiation (server selects http/1.1)

TEST(HttpTlsMtlsAlpn, RequireClientCertHandshakeFailsWithout) {
  auto serverCert = aeronet::test::makeEphemeralCertKey();  // still needed for trust store
  ASSERT_FALSE(serverCert.first.empty());
  ASSERT_FALSE(serverCert.second.empty());
  std::string resp;
  std::string alpn;
  {
    aeronet::test::TlsTestServer ts({"http/1.1"}, [&](aeronet::HttpServerConfig& cfg) {
      cfg.withTlsRequireClientCert(true).withTlsAddTrustedClientCert(serverCert.first);
    });
    auto port = ts.port();
    ts.setDefault([](const aeronet::HttpRequest& req) {
      return aeronet::HttpResponse(aeronet::http::StatusCodeOK)
          .reason("OK")
          .contentType(aeronet::http::ContentTypeTextPlain)
          .body(std::string("SECURE") + std::string(req.path()));
    });
    aeronet::test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    // No client cert provided, so handshake should fail due to required client cert.
    aeronet::test::TlsClient client(port, opts);
    if (client.handshakeOk()) {
      resp = client.get("/secure");
      alpn = client.negotiatedAlpn();
    }
    ts.stop();
  }
  // Expect empty response (handshake failed or connection closed before HTTP response)
  ASSERT_TRUE(resp.empty());
}

TEST(HttpTlsMtlsAlpn, RequireClientCertSuccessWithAlpn) {
  auto serverCert = aeronet::test::makeEphemeralCertKey();
  ASSERT_FALSE(serverCert.first.empty());
  ASSERT_FALSE(serverCert.second.empty());
  auto clientCert = serverCert;  // reuse same self-signed for simplicity
  std::string resp;
  std::string alpn;
  {
    aeronet::test::TlsTestServer ts({"http/1.1"}, [&](aeronet::HttpServerConfig& cfg) {
      cfg.withTlsRequireClientCert(true).withTlsAddTrustedClientCert(clientCert.first);
    });
    auto port = ts.port();
    ts.setDefault([](const aeronet::HttpRequest& req) {
      return aeronet::HttpResponse(aeronet::http::StatusCodeOK)
          .reason("OK")
          .contentType(aeronet::http::ContentTypeTextPlain)
          .body(std::string("SECURE") + std::string(req.path()));
    });
    aeronet::test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    opts.clientCertPem = clientCert.first;
    opts.clientKeyPem = clientCert.second;
    aeronet::test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    resp = client.get("/secure");
    alpn = client.negotiatedAlpn();
    ts.stop();
  }
  ASSERT_FALSE(resp.empty());
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("SECURE/secure"));
  ASSERT_EQ(alpn, "http/1.1");
}

TEST(HttpTlsCipherVersion, CipherAndVersionExposedAndMetricsIncrement) {
  // Metrics now per-server; no global reset needed.
  // TLS fixture auto-generates cert/key; request ALPN http/1.1
  std::string capturedCipher;
  std::string capturedVersion;
  std::string capturedAlpn;
  aeronet::ServerStats statsSnapshot{};
  {
    aeronet::test::TlsTestServer ts({"http/1.1"});
    auto port = ts.port();
    ts.setDefault([&](const aeronet::HttpRequest& req) {
      capturedCipher = std::string(req.tlsCipher());
      capturedVersion = std::string(req.tlsVersion());
      capturedAlpn = std::string(req.alpnProtocol());

      return aeronet::HttpResponse(200, "OK").contentType(aeronet::http::ContentTypeTextPlain).body("ok");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));  // allow handshake path if needed
    aeronet::test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    aeronet::test::TlsClient client(port, opts);
    auto resp = client.get("/");
    ts.stop();
    ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
    statsSnapshot = ts.stats();
    ASSERT_GE(statsSnapshot.tlsHandshakesSucceeded, 1U);
    ASSERT_EQ(statsSnapshot.tlsAlpnStrictMismatches, 0U);
  }
  ASSERT_FALSE(capturedCipher.empty());
  ASSERT_FALSE(capturedVersion.empty());
  // ALPN was offered and should match
  ASSERT_EQ(capturedAlpn, "http/1.1");
  // Distribution should show at least one for http/1.1
  bool found = false;
  for (const auto& kv : statsSnapshot.tlsAlpnDistribution) {
    if (kv.first == "http/1.1") {
      found = true;
      ASSERT_GE(kv.second, 1U);
    }
  }
  ASSERT_TRUE(found);
  ASSERT_EQ(statsSnapshot.tlsClientCertPresent, 0U);  // no client cert
  // Assert cipher & version distributions contain negotiated values
  bool cipherFound = false;
  for (const auto& kv : statsSnapshot.tlsCipherCounts) {
    if (kv.first == capturedCipher) {
      cipherFound = true;
      ASSERT_GE(kv.second, 1U);
    }
  }
  ASSERT_TRUE(cipherFound);
  bool versionFound = false;
  for (const auto& kv : statsSnapshot.tlsVersionCounts) {
    if (kv.first == capturedVersion) {
      versionFound = true;
      ASSERT_GE(kv.second, 1U);
    }
  }
  ASSERT_TRUE(versionFound);
  // Handshake duration metrics populated
  ASSERT_GE(statsSnapshot.tlsHandshakeDurationCount, 1U);
  ASSERT_GE(statsSnapshot.tlsHandshakeDurationTotalNs, statsSnapshot.tlsHandshakeDurationMaxNs);
  ASSERT_GT(statsSnapshot.tlsHandshakeDurationMaxNs, 0U);
}

TEST(HttpTlsCipherList, InvalidCipherListThrows) {
  EXPECT_THROW(
      {
        aeronet::test::TlsTestServer ts(
            {}, [](aeronet::HttpServerConfig& cfg) { cfg.withTlsCipherList("INVALID-CIPHER-1234"); });
      },
      std::runtime_error);
}

TEST(HttpTlsFileCertKey, HandshakeSucceedsUsingFileBasedCertAndKey) {
  auto pair = aeronet::test::makeEphemeralCertKey();
  ASSERT_FALSE(pair.first.empty());
  ASSERT_FALSE(pair.second.empty());
  // Write both to temp files
  auto certFile = TempFile::createWithContent("aeronet_cert_", pair.first);
  auto keyFile = TempFile::createWithContent("aeronet_key_", pair.second);
  ASSERT_TRUE(certFile.valid());
  ASSERT_TRUE(keyFile.valid());

  aeronet::HttpServerConfig cfg;
  cfg.withTlsCertKey(certFile.path(), keyFile.path());  // file-based path (not memory)
  cfg.withTlsAlpnProtocols({"http/1.1"});
  // Use plain TestServer since we manually set config
  aeronet::test::TestServer server(cfg, std::chrono::milliseconds{50});
  server.server.router().setDefault([](const aeronet::HttpRequest& req) {
    return aeronet::HttpResponse(200, "OK")
        .contentType(aeronet::http::ContentTypeTextPlain)
        .body(std::string("FILETLS-") + std::string(req.alpnProtocol().empty() ? "-" : req.alpnProtocol()));
  });
  uint16_t port = server.port();

  aeronet::test::TlsClient::Options opts;
  opts.alpn = {"http/1.1"};
  aeronet::test::TlsClient client(port, opts);
  ASSERT_TRUE(client.handshakeOk());
  auto resp = client.get("/file");
  server.stop();
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("FILETLS-http/1.1"));
}

TEST(HttpTlsMtlsMetrics, ClientCertPresenceIncrementsMetric) {
  // Per-server metrics now, no global reset required.
  auto certKey = aeronet::test::makeEphemeralCertKey();  // also used as trusted client CA
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());
  {
    aeronet::test::TlsTestServer ts({"http/1.1"}, [&](aeronet::HttpServerConfig& cfg) {
      cfg.withTlsRequireClientCert(true).withTlsAddTrustedClientCert(certKey.first);
    });
    auto port = ts.port();
    ts.setDefault([](const aeronet::HttpRequest&) {
      return aeronet::HttpResponse(200, "OK").contentType(aeronet::http::ContentTypeTextPlain).body("m");
    });
    auto before = ts.stats();
    aeronet::test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    opts.clientCertPem = certKey.first;
    opts.clientKeyPem = certKey.second;
    aeronet::test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto resp = client.get("/m");
    auto after = ts.stats();
    ts.stop();
    ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
    ASSERT_LT(before.tlsClientCertPresent, after.tlsClientCertPresent);
    ASSERT_GE(after.tlsHandshakesSucceeded, 1U);
  }
}

namespace {
// Large response GET using TlsClient (simplified replacement).
std::string tlsGetLarge(auto port) {
  aeronet::test::TlsClient client(port);
  if (!client.handshakeOk()) {
    return {};
  }
  return client.get("/large");
}
}  // namespace

TEST(HttpTlsNegative, PlainHttpToTlsPortRejected) {
  // perform a raw TCP connect and send cleartext HTTP to a TLS-only port -> should fail handshake quickly.

  aeronet::test::TlsTestServer ts;  // default TLS (no ALPN needed here)

  aeronet::test::ClientConnection cnx(ts.port());
  int fd = cnx.fd();

  std::string bogus = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";  // not TLS handshake
  ASSERT_TRUE(aeronet::test::sendAll(fd, bogus));
  // default recvWithTimeout waits 2000ms; explicit shorter timeout keeps test fast
  ASSERT_TRUE(aeronet::test::recvWithTimeout(fd, 500ms).empty());
}

// When server only requests (but does not require) a client cert, handshake should succeed
// whether or not the client presents one; metric tlsClientCertPresent should reflect presence.

TEST(HttpTlsRequestClientCert, OptionalNoClientCertAccepted) {
  std::string body;
  aeronet::ServerStats statsAfter{};
  {
    aeronet::test::TlsTestServer ts({}, [](aeronet::HttpServerConfig& cfg) { cfg.withTlsRequestClientCert(true); });
    auto port = ts.port();
    ts.setDefault([&](const aeronet::HttpRequest& req) {
      aeronet::HttpResponse resp(200);
      resp.reason("OK");
      resp.contentType(aeronet::http::ContentTypeTextPlain);
      if (!req.tlsCipher().empty()) {
        resp.body(std::string("REQ-") + std::string(req.tlsCipher()));
      } else {
        resp.body("REQ-");
      }
      return resp;
    });
    aeronet::test::TlsClient client(port);  // no client cert
    ASSERT_TRUE(client.handshakeOk());
    body = client.get("/nocert");
    statsAfter = ts.stats();
    ts.stop();
  }
  ASSERT_TRUE(body.contains("HTTP/1.1 200"));
  ASSERT_EQ(statsAfter.tlsClientCertPresent, 0U);
  ASSERT_GE(statsAfter.tlsHandshakesSucceeded, 1U);
}

TEST(HttpTlsRequestClientCert, OptionalWithClientCertIncrementsMetric) {
  auto clientPair = aeronet::test::makeEphemeralCertKey();
  ASSERT_FALSE(clientPair.first.empty());
  ASSERT_FALSE(clientPair.second.empty());
  aeronet::ServerStats statsAfter{};
  {
    // Trust the self-signed client cert for verification if sent; but handshake must still succeed w/out require flag.
    aeronet::test::TlsTestServer ts({}, [&](aeronet::HttpServerConfig& cfg) {
      cfg.withTlsRequestClientCert(true).withTlsAddTrustedClientCert(clientPair.first);
    });
    auto port = ts.port();
    ts.setDefault([](const aeronet::HttpRequest&) {
      return aeronet::HttpResponse()
          .statusCode(200)
          .reason("OK")
          .contentType(aeronet::http::ContentTypeTextPlain)
          .body("C");
    });
    aeronet::test::TlsClient::Options opts;
    opts.clientCertPem = clientPair.first;
    opts.clientKeyPem = clientPair.second;
    aeronet::test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto response = client.get("/withcert");
    statsAfter = ts.stats();
    ts.stop();
    ASSERT_TRUE(response.contains("HTTP/1.1 200"));
  }
  ASSERT_GE(statsAfter.tlsHandshakesSucceeded, 1U);
  ASSERT_EQ(statsAfter.tlsClientCertPresent, 1U);
}

TEST(HttpTlsVersionBounds, MinMaxTls12Forces12) {
  std::string capturedVersion;
  aeronet::ServerStats statsAfter{};
  {
    aeronet::test::TlsTestServer ts({"http/1.1"}, [](aeronet::HttpServerConfig& cfg) {
      cfg.withTlsMinVersion("TLS1.2").withTlsMaxVersion("TLS1.2");
    });
    auto port = ts.port();
    ts.setDefault([&](const aeronet::HttpRequest& req) {
      if (!req.tlsVersion().empty()) {
        capturedVersion = std::string(req.tlsVersion());
      }

      return aeronet::HttpResponse(200, "OK").contentType(aeronet::http::ContentTypeTextPlain).body("V");
    });
    aeronet::test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    aeronet::test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto resp = client.get("/v");
    statsAfter = ts.stats();
    ts.stop();
    ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  }
  ASSERT_FALSE(capturedVersion.empty());
  // OpenSSL commonly returns "TLSv1.2"; accept any token containing 1.2
  ASSERT_TRUE(capturedVersion.contains("1.2"));
  bool found = false;
  // Only iterate version counts if OpenSSL enabled (members present)
  for (const auto& kv : statsAfter.tlsVersionCounts) {
    if (kv.first == capturedVersion) {
      found = true;
      ASSERT_GE(kv.second, 1U);
    }
  }
  ASSERT_TRUE(found);
}

TEST(HttpTlsVersionBounds, InvalidMinVersionThrows) {
  // Provide invalid version string -> expect construction failure.
  EXPECT_THROW(
      { aeronet::test::TlsTestServer ts({}, [](aeronet::HttpServerConfig& cfg) { cfg.withTlsMinVersion("TLS1.1"); }); },
      aeronet::invalid_argument);
}