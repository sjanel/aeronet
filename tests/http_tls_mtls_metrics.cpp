#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "test_server_tls_fixture.hpp"
#include "test_tls_client.hpp"
#include "test_tls_helper.hpp"

TEST(HttpTlsMtlsMetrics, ClientCertPresenceIncrementsMetric) {
  // Per-server metrics now, no global reset required.
  auto certKey = aeronet::test::makeEphemeralCertKey();  // also used as trusted client CA
  ASSERT_FALSE(certKey.first.empty());
  ASSERT_FALSE(certKey.second.empty());
  {
    TlsTestServer ts({"http/1.1"}, [&](aeronet::ServerConfig& cfg) {
      cfg.withTlsRequireClientCert(true).withTlsAddTrustedClientCert(certKey.first);
    });
    auto port = ts.port();
    ts.setHandler([](const aeronet::HttpRequest&) {
      aeronet::HttpResponse respOut;
      respOut.statusCode = 200;
      respOut.reason = "OK";
      respOut.contentType = "text/plain";
      respOut.body = "m";
      return respOut;
    });
    [[maybe_unused]] auto before = ts.stats();
    TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    opts.clientCertPem = certKey.first;
    opts.clientKeyPem = certKey.second;
    TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto resp = client.get("/m");
    [[maybe_unused]] auto after = ts.stats();
    ts.stop();
    ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
    ASSERT_LT(before.tlsClientCertPresent, after.tlsClientCertPresent);
    ASSERT_GE(after.tlsHandshakesSucceeded, 1U);
  }
}
