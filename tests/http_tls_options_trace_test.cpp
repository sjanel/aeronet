#include <gtest/gtest.h>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"

using namespace aeronet;

TEST(HttpOptionsTraceTls, TraceDisabledOnTlsPolicyRejectsTlsTrace) {
  using namespace aeronet::test;
  // Use TlsTestServer and set TracePolicy to EnabledPlainOnly (reject TRACE over TLS)
  TlsTestServer ts(
      {}, [](HttpServerConfig& cfg) { cfg.withTracePolicy(HttpServerConfig::TraceMethodPolicy::EnabledPlainOnly); });

  // Default handler (not needed but keep server alive)
  ts.setDefault([](const aeronet::HttpRequest&) { return aeronet::HttpResponse(200); });

  // Use a TLS client to send a TRACE request; it should be rejected (405)
  TlsClient client(ts.port());
  ASSERT_TRUE(client.handshakeOk());
  client.writeAll("TRACE /test HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  auto raw = client.readAll();
  ASSERT_TRUE(raw.contains("405")) << raw;
}

TEST(HttpOptionsTraceTls, TraceEnabledOnTlsAllowsTlsTrace) {
  using namespace aeronet::test;
  // EnabledPlainAndTLS should allow TRACE over TLS
  TlsTestServer ts(
      {}, [](HttpServerConfig& cfg) { cfg.withTracePolicy(HttpServerConfig::TraceMethodPolicy::EnabledPlainAndTLS); });
  ts.setDefault([](const aeronet::HttpRequest&) { return aeronet::HttpResponse(200); });

  TlsClient client(ts.port());
  ASSERT_TRUE(client.handshakeOk());
  client.writeAll("TRACE /test HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  auto raw = client.readAll();
  ASSERT_TRUE(raw.contains("200")) << raw;
}
