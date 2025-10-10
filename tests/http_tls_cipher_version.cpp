#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"

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
    ts.setHandler([&](const aeronet::HttpRequest& req) {
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
    ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
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
