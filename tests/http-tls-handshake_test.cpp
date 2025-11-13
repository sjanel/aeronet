#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"
#include "aeronet/test_tls_helper.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;
using namespace std::chrono_literals;

TEST(HttpTlsBasic, HandshakeAndSimpleGet) {
  // Prepare config with in-memory self-signed cert/key
  test::TlsTestServer ts;  // ephemeral TLS server
  ts.setDefault([](const HttpRequest& req) {
    return HttpResponse(http::StatusCodeOK, "OK")

        .body(std::string("TLS OK ") + std::string(req.path()));
  });
  test::TlsClient client(ts.port());
  auto raw = client.get("/hello", {{"X-Test", "tls"}});
  ASSERT_FALSE(raw.empty());
  ASSERT_TRUE(raw.contains("HTTP/1.1 200"));
  ASSERT_TRUE(raw.contains("TLS OK /hello"));
}

TEST(HttpTlsAlpnMismatch, HandshakeFailsWhenNoCommonProtocolAndMustMatch) {
  bool failed = false;
  ServerStats statsAfter{};
  {
    test::TlsTestServer ts({"http/1.1", "h2"}, [](HttpServerConfig& cfg) { cfg.withTlsAlpnMustMatch(true); });
    auto port = ts.port();
    ts.setDefault([](const HttpRequest& req) {
      return HttpResponse(http::StatusCodeOK)
          .reason("OK")

          .body(std::string("ALPN:") + std::string(req.alpnProtocol()));
    });
    // Offer only a mismatching ALPN; since TlsClient uses options, construct with protoX.
    test::TlsClient::Options opts;
    opts.alpn = {"protoX"};
    test::TlsClient client(port, opts);
    failed = !client.handshakeOk();
    statsAfter = ts.stats();
    ts.stop();
  }
  ASSERT_TRUE(failed);
  ASSERT_GE(statsAfter.tlsAlpnStrictMismatches, 1U);
}

TEST(HttpTlsAlpnNonStrict, MismatchAllowedAndNoMetricIncrement) {
  std::string capturedAlpn;
  ServerStats statsAfter{};
  {
    // Server prefers h2, but does NOT enforce match.
    test::TlsTestServer ts({"h2"});
    auto port = ts.port();
    ts.setDefault([&](const HttpRequest& req) {
      if (!req.alpnProtocol().empty()) {
        capturedAlpn = std::string(req.alpnProtocol());
      } else {
        capturedAlpn.clear();
      }
      return HttpResponse(200, "OK").body("NS");
    });
    test::TlsClient::Options opts;
    opts.alpn = {"foo"};  // no overlap
    test::TlsClient client(port, opts);
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
  auto pair = test::makeEphemeralCertKey();
  ASSERT_FALSE(pair.first.empty());
  ASSERT_FALSE(pair.second.empty());

  HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(pair.first, pair.second);
  cfg.withTlsAlpnProtocols({"h2", "http/1.1"});  // offer both; client will request http/1.1 only

  HttpServer original(cfg);
  original.router().setDefault([](const HttpRequest& req) {
    return HttpResponse(http::StatusCodeOK, "OK")

        .body(std::string("MOVEALPN:") + (req.alpnProtocol().empty() ? "-" : std::string(req.alpnProtocol())));
  });

  auto port = original.port();
  HttpServer moved(std::move(original));

  std::atomic_bool stop{false};
  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });

  // Actively wait until the listening socket accepts a plain TCP connection to avoid race.
  // This replicates TestServer readiness logic without duplicating its wrapper.
  {
    test::ClientConnection probe(port, std::chrono::milliseconds{500});
  }

  test::TlsClient::Options opts;
  opts.alpn = {"http/1.1"};
  test::TlsClient client(port, opts);
  ASSERT_TRUE(client.handshakeOk()) << "TLS handshake failed after move (potential stale TlsContext pointer)";
  auto raw = client.get("/moved");
  stop.store(true);

  ASSERT_TRUE(raw.contains("HTTP/1.1 200"));
  ASSERT_TRUE(raw.contains("MOVEALPN:http/1.1")) << raw;
}

// Test mutual TLS requirement and ALPN negotiation (server selects http/1.1)

TEST(HttpTlsMtlsAlpn, RequireClientCertHandshakeFailsWithout) {
  auto serverCert = test::makeEphemeralCertKey();  // still needed for trust store
  ASSERT_FALSE(serverCert.first.empty());
  ASSERT_FALSE(serverCert.second.empty());
  std::string resp;
  std::string alpn;
  {
    test::TlsTestServer ts({"http/1.1"}, [&](HttpServerConfig& cfg) {
      cfg.withTlsRequireClientCert(true).withTlsTrustedClientCert(serverCert.first);
    });
    auto port = ts.port();
    ts.setDefault([](const HttpRequest& req) {
      return HttpResponse(http::StatusCodeOK)
          .reason("OK")

          .body(std::string("SECURE") + std::string(req.path()));
    });
    test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    // No client cert provided, so handshake should fail due to required client cert.
    test::TlsClient client(port, opts);
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
  auto serverCert = test::makeEphemeralCertKey();
  ASSERT_FALSE(serverCert.first.empty());
  ASSERT_FALSE(serverCert.second.empty());
  auto clientCert = serverCert;  // reuse same self-signed for simplicity
  std::string resp;
  std::string alpn;
  {
    test::TlsTestServer ts({"http/1.1"}, [&](HttpServerConfig& cfg) {
      cfg.withTlsRequireClientCert(true).withTlsTrustedClientCert(clientCert.first);
    });
    auto port = ts.port();
    ts.setDefault([](const HttpRequest& req) {
      return HttpResponse(http::StatusCodeOK)
          .reason("OK")

          .body(std::string("SECURE") + std::string(req.path()));
    });
    test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    opts.clientCertPem = clientCert.first;
    opts.clientKeyPem = clientCert.second;
    test::TlsClient client(port, opts);
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
  ServerStats statsSnapshot{};
  {
    test::TlsTestServer ts({"http/1.1"});
    auto port = ts.port();
    ts.setDefault([&](const HttpRequest& req) {
      capturedCipher = std::string(req.tlsCipher());
      capturedVersion = std::string(req.tlsVersion());
      capturedAlpn = std::string(req.alpnProtocol());

      return HttpResponse(200, "OK").body("ok");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));  // allow handshake path if needed
    test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    test::TlsClient client(port, opts);
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
      { test::TlsTestServer ts({}, [](HttpServerConfig& cfg) { cfg.withTlsCipherList("INVALID-CIPHER-1234"); }); },
      std::runtime_error);
}

TEST(HttpTlsFileCertKey, HandshakeSucceedsUsingFileBasedCertAndKey) {
  auto pair = test::makeEphemeralCertKey();
  ASSERT_FALSE(pair.first.empty());
  ASSERT_FALSE(pair.second.empty());
  // Write both to temp files inside a temporary directory
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile certFile(tmpDir, pair.first);
  test::ScopedTempFile keyFile(tmpDir, pair.second);

  HttpServerConfig cfg;
  cfg.withTlsCertKey(certFile.filePath().string(), keyFile.filePath().string());  // file-based path (not memory)
  cfg.withTlsAlpnProtocols({"http/1.1"});
  // Use plain TestServer since we manually set config
  test::TestServer server(cfg, RouterConfig{}, std::chrono::milliseconds{50});
  server.router().setDefault([](const HttpRequest& req) {
    return HttpResponse(200, "OK")

        .body(std::string("FILETLS-") + std::string(req.alpnProtocol().empty() ? "-" : req.alpnProtocol()));
  });
  uint16_t port = server.port();

  test::TlsClient::Options opts;
  opts.alpn = {"http/1.1"};
  test::TlsClient client(port, opts);
  ASSERT_TRUE(client.handshakeOk());
  auto resp = client.get("/file");
  server.stop();
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("FILETLS-http/1.1"));
}

TEST(HttpTlsMtlsMetrics, ClientCertPresenceIncrementsMetric) {
  // Per-server metrics now, no global reset required.
  auto certKey = test::makeEphemeralCertKey();  // also used as trusted client CA
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());
  {
    test::TlsTestServer ts({"http/1.1"}, [&](HttpServerConfig& cfg) {
      cfg.withTlsRequireClientCert(true).withTlsTrustedClientCert(certKey.first);
    });
    auto port = ts.port();
    ts.setDefault([](const HttpRequest&) { return HttpResponse(200, "OK").body("m"); });
    auto before = ts.stats();
    test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    opts.clientCertPem = certKey.first;
    opts.clientKeyPem = certKey.second;
    test::TlsClient client(port, opts);
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
  test::TlsClient client(port);
  if (!client.handshakeOk()) {
    return {};
  }
  return client.get("/large");
}
}  // namespace

TEST(HttpTlsNegative, PlainHttpToTlsPortRejected) {
  // perform a raw TCP connect and send cleartext HTTP to a TLS-only port -> should fail handshake quickly.

  test::TlsTestServer ts;  // default TLS (no ALPN needed here)

  test::ClientConnection cnx(ts.port());
  int fd = cnx.fd();

  std::string bogus = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";  // not TLS handshake
  test::sendAll(fd, bogus);
  // default recvWithTimeout waits 2000ms; explicit shorter timeout keeps test fast
  ASSERT_TRUE(test::recvWithTimeout(fd, 500ms).empty());
}

// When server only requests (but does not require) a client cert, handshake should succeed
// whether or not the client presents one; metric tlsClientCertPresent should reflect presence.

TEST(HttpTlsRequestClientCert, OptionalNoClientCertAccepted) {
  std::string body;
  ServerStats statsAfter{};
  {
    test::TlsTestServer ts({}, [](HttpServerConfig& cfg) { cfg.withTlsRequestClientCert(true); });
    auto port = ts.port();
    ts.setDefault([&](const HttpRequest& req) {
      HttpResponse resp(200);
      resp.reason("OK");
      if (!req.tlsCipher().empty()) {
        resp.body(std::string("REQ-") + std::string(req.tlsCipher()));
      } else {
        resp.body("REQ-");
      }
      return resp;
    });
    test::TlsClient client(port);  // no client cert
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
  auto clientPair = test::makeEphemeralCertKey();
  ASSERT_FALSE(clientPair.first.empty());
  ASSERT_FALSE(clientPair.second.empty());
  ServerStats statsAfter{};
  {
    // Trust the self-signed client cert for verification if sent; but handshake must still succeed w/out require flag.
    test::TlsTestServer ts({}, [&](HttpServerConfig& cfg) {
      cfg.withTlsRequestClientCert(true).withTlsTrustedClientCert(clientPair.first);
    });
    auto port = ts.port();
    ts.setDefault([](const HttpRequest&) { return HttpResponse().status(http::StatusCodeOK, "OK").body("C"); });
    test::TlsClient::Options opts;
    opts.clientCertPem = clientPair.first;
    opts.clientKeyPem = clientPair.second;
    test::TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto response = client.get("/withcert");
    statsAfter = ts.stats();
    ts.stop();
    ASSERT_TRUE(response.contains("HTTP/1.1 200"));
  }
  ASSERT_GE(statsAfter.tlsHandshakesSucceeded, 1U);
  ASSERT_EQ(statsAfter.tlsClientCertPresent, 1U);
}

TEST(HttpTlsHandshakeTimeout, SilentClientClosed) {
  constexpr auto handshakeTimeout = std::chrono::milliseconds{40};
  test::TlsTestServer ts({}, [&](HttpServerConfig& cfg) {
    cfg.withTlsHandshakeTimeout(handshakeTimeout);
    cfg.withPollInterval(std::chrono::milliseconds{5});
  });

  test::ClientConnection cnx(ts.port());
  const int fd = cnx.fd();
  ASSERT_GE(fd, 0) << "connect failed";

  const bool closed = test::WaitForPeerClose(fd, handshakeTimeout * 6);
  ts.stop();
  EXPECT_TRUE(closed);
}

TEST(HttpTlsHandshakeTimeout, SuccessfulHandshakeUnaffected) {
  test::TlsTestServer ts({},
                         [&](HttpServerConfig& cfg) { cfg.withTlsHandshakeTimeout(std::chrono::milliseconds{200}); });
  ts.setDefault([](const HttpRequest&) { return HttpResponse(200, "OK").body("handshake-ok"); });

  test::TlsClient client(ts.port());
  ASSERT_TRUE(client.handshakeOk());
  const auto resp = client.get("/ok");
  ts.stop();

  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("handshake-ok"));
}

TEST(HttpTlsVersionBounds, MinMaxTls12Forces12) {
  std::string capturedVersion;
  ServerStats statsAfter{};
  {
    test::TlsTestServer ts({"http/1.1"},
                           [](HttpServerConfig& cfg) { cfg.withTlsMinVersion("TLS1.2").withTlsMaxVersion("TLS1.2"); });
    auto port = ts.port();
    ts.setDefault([&](const HttpRequest& req) {
      if (!req.tlsVersion().empty()) {
        capturedVersion = std::string(req.tlsVersion());
      }

      return HttpResponse(200, "OK").body("V");
    });
    test::TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    test::TlsClient client(port, opts);
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
