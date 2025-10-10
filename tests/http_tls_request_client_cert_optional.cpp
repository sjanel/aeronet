// Tests optional (request-only) client certificate behavior (no mTLS requirement)
#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"
#include "aeronet/test_tls_helper.hpp"

// When server only requests (but does not require) a client cert, handshake should succeed
// whether or not the client presents one; metric tlsClientCertPresent should reflect presence.

TEST(HttpTlsRequestClientCert, OptionalNoClientCertAccepted) {
  std::string body;
  aeronet::ServerStats statsAfter{};
  {
    aeronet::test::TlsTestServer ts({}, [](aeronet::HttpServerConfig& cfg) { cfg.withTlsRequestClientCert(true); });
    auto port = ts.port();
    ts.setHandler([&](const aeronet::HttpRequest& req) {
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
  ASSERT_NE(std::string::npos, body.find("HTTP/1.1 200"));
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
    ts.setHandler([](const aeronet::HttpRequest&) {
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
    ASSERT_NE(std::string::npos, response.find("HTTP/1.1 200"));
  }
  ASSERT_GE(statsAfter.tlsHandshakesSucceeded, 1U);
  ASSERT_EQ(statsAfter.tlsClientCertPresent, 1U);
}
