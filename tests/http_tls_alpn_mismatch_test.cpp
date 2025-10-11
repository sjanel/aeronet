#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"

TEST(HttpTlsAlpnMismatch, HandshakeFailsWhenNoCommonProtocolAndMustMatch) {
  bool failed = false;
  aeronet::ServerStats statsAfter{};
  {
    aeronet::test::TlsTestServer ts({"http/1.1", "h2"},
                                    [](aeronet::HttpServerConfig& cfg) { cfg.withTlsAlpnMustMatch(true); });
    auto port = ts.port();
    ts.setDefault([](const aeronet::HttpRequest& req) {
      return aeronet::HttpResponse(200)
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
