// ALPN mismatch when not in strict mode should still allow handshake; ALPN result empty.
#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-stats.hpp"
#include "http-constants.hpp"
#include "test_server_tls_fixture.hpp"
#include "test_tls_client.hpp"

TEST(HttpTlsAlpnNonStrict, MismatchAllowedAndNoMetricIncrement) {
  std::string capturedAlpn;
  aeronet::ServerStats statsAfter{};
  {
    // Server prefers h2, but does NOT enforce match.
    TlsTestServer ts({"h2"});
    auto port = ts.port();
    ts.setHandler([&](const aeronet::HttpRequest& req) {
      if (!req.alpnProtocol().empty()) {
        capturedAlpn = std::string(req.alpnProtocol());
      } else {
        capturedAlpn.clear();
      }
      return aeronet::HttpResponse(200, "OK").contentType(aeronet::http::ContentTypeTextPlain).body("NS");
    });
    TlsClient::Options opts;
    opts.alpn = {"foo"};  // no overlap
    TlsClient client(port, opts);
    ASSERT_TRUE(client.handshakeOk());
    auto resp = client.get("/non_strict");
    statsAfter = ts.stats();
    ts.stop();
    ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
  }
  // ALPN not negotiated => empty string
  ASSERT_TRUE(capturedAlpn.empty());
  ASSERT_EQ(statsAfter.tlsAlpnStrictMismatches, 0U);
  // Distribution should NOT contain client-only protocol
  for (const auto& kv : statsAfter.tlsAlpnDistribution) {
    ASSERT_NE(kv.first, std::string("foo"));
  }
}
