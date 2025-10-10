#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"
#include "aeronet/test_tls_helper.hpp"

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
    ts.setHandler([](const aeronet::HttpRequest&) {
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
    ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
    ASSERT_LT(before.tlsClientCertPresent, after.tlsClientCertPresent);
    ASSERT_GE(after.tlsHandshakesSucceeded, 1U);
  }
}
