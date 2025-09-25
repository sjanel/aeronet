#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "test_server_tls_fixture.hpp"
#include "test_tls_client.hpp"
#include "test_tls_helper.hpp"

// Test mutual TLS requirement and ALPN negotiation (server selects http/1.1)

TEST(HttpTlsMtlsAlpn, RequireClientCertHandshakeFailsWithout) {
  auto serverCert = aeronet::test::makeEphemeralCertKey();  // still needed for trust store
  ASSERT_FALSE(serverCert.first.empty());
  ASSERT_FALSE(serverCert.second.empty());
  std::string resp;
  std::string alpn;
  {
    TlsTestServer ts({"http/1.1"}, [&](aeronet::HttpServerConfig& cfg) {
      cfg.withTlsRequireClientCert(true).withTlsAddTrustedClientCert(serverCert.first);
    });
    auto port = ts.port();
    ts.setHandler([](const aeronet::HttpRequest& req) {
      aeronet::HttpResponse resp;
      resp.statusCode = 200;
      resp.reason = "OK";
      resp.contentType = "text/plain";
      resp.body = std::string("SECURE") + std::string(req.target);
      return resp;
    });
    TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    // No client cert provided, so handshake should fail due to required client cert.
    TlsClient client(port, opts);
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
    TlsTestServer ts({"http/1.1"}, [&](aeronet::HttpServerConfig& cfg) {
      cfg.withTlsRequireClientCert(true).withTlsAddTrustedClientCert(clientCert.first);
    });
    auto port = ts.port();
    ts.setHandler([](const aeronet::HttpRequest& req) {
      aeronet::HttpResponse resp;
      resp.statusCode = 200;
      resp.reason = "OK";
      resp.contentType = "text/plain";
      resp.body = std::string("SECURE") + std::string(req.target);
      return resp;
    });
    TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    opts.clientCertPem = clientCert.first;
    opts.clientKeyPem = clientCert.second;
    TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    resp = client.get("/secure");
    alpn = client.negotiatedAlpn();
    ts.stop();
  }
  ASSERT_FALSE(resp.empty());
  ASSERT_NE(resp.find("HTTP/1.1 200"), std::string::npos);
  ASSERT_NE(resp.find("SECURE/secure"), std::string::npos);
  ASSERT_EQ(alpn, "http/1.1");
}
