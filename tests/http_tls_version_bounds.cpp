// Tests TLS version bounds configuration (min/max) and invalid version handling.
#include <gtest/gtest.h>

#include <stdexcept>  // std::runtime_error
#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/server-stats.hpp"  // aeronet::ServerStats
#include "test_server_tls_fixture.hpp"
#include "test_tls_client.hpp"

TEST(HttpTlsVersionBounds, MinMaxTls12Forces12) {
  std::string capturedVersion;
  aeronet::ServerStats statsAfter{};
  {
    TlsTestServer ts({"http/1.1"}, [](aeronet::HttpServerConfig& cfg) {
      cfg.withTlsMinVersion("TLS1.2").withTlsMaxVersion("TLS1.2");
    });
    auto port = ts.port();
    ts.setHandler([&](const aeronet::HttpRequest& req) {
      if (!req.tlsVersion().empty()) {
        capturedVersion = std::string(req.tlsVersion());
      }

      return aeronet::HttpResponse(200, "OK").contentType(aeronet::http::ContentTypeTextPlain).body("V");
    });
    TlsClient::Options opts;
    opts.alpn = {"http/1.1"};
    TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto resp = client.get("/v");
    statsAfter = ts.stats();
    ts.stop();
    ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
  }
  ASSERT_FALSE(capturedVersion.empty());
  // OpenSSL commonly returns "TLSv1.2"; accept any token containing 1.2
  ASSERT_NE(capturedVersion.find("1.2"), std::string::npos);
  bool found = false;
  // Only iterate version counts if OpenSSL enabled (members present)
#ifdef AERONET_ENABLE_OPENSSL
  for (const auto& kv : statsAfter.tlsVersionCounts) {
    if (kv.first == capturedVersion) {
      found = true;
      ASSERT_GE(kv.second, 1U);
    }
  }
#endif
  ASSERT_TRUE(found);
}

TEST(HttpTlsVersionBounds, InvalidMinVersionThrows) {
  // Provide invalid version string -> expect construction failure.
  EXPECT_THROW(
      { TlsTestServer ts({}, [](aeronet::HttpServerConfig& cfg) { cfg.withTlsMinVersion("TLS1.1"); }); },
      std::runtime_error);
}
