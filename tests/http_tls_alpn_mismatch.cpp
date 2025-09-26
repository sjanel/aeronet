#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/server-stats.hpp"
#include "test_server_tls_fixture.hpp"
#include "test_tls_client.hpp"

TEST(HttpTlsAlpnMismatch, HandshakeFailsWhenNoCommonProtocolAndMustMatch) {
  bool failed = false;
  aeronet::ServerStats statsAfter{};
  {
    TlsTestServer ts({"http/1.1", "h2"}, [](aeronet::HttpServerConfig& cfg) { cfg.withTlsAlpnMustMatch(true); });
    auto port = ts.port();
    ts.setHandler([](const aeronet::HttpRequest& req) {
      aeronet::HttpResponse resp;
      resp.statusCode = 200;
      resp.reason = "OK";
      resp.contentType = "text/plain";
      resp.body = std::string("ALPN:") + std::string(req.alpnProtocol);
      return resp;
    });
    // Offer only a mismatching ALPN; since TlsClient uses options, construct with protoX.
    TlsClient::Options opts;
    opts.alpn = {"protoX"};
    TlsClient client(port, opts);
    failed = !client.handshakeOk();
    statsAfter = ts.stats();
    ts.stop();
  }
  ASSERT_TRUE(failed);
  ASSERT_GE(statsAfter.tlsAlpnStrictMismatches, 1U);
}
