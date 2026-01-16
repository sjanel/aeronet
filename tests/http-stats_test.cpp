#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

TEST(HttpStats, BasicCountersIncrement) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(5);
  test::TestServer ts(cfg);
  ts.router().setDefault([]([[maybe_unused]] const HttpRequest& req) { return HttpResponse(200).body("hello"); });
  // Single request via throwing helper
  auto resp = test::requestOrThrow(ts.port());
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ts.stop();
  auto st = ts.server.stats();
  EXPECT_GT(st.totalBytesQueued, 0U);  // headers+body accounted
  EXPECT_GT(st.totalBytesWrittenImmediate + st.totalBytesWrittenFlush, 0U);
  EXPECT_GE(st.maxConnectionOutboundBuffer, 0U);
  EXPECT_GE(st.flushCycles, 0U);
}

// Test that ServerStats::json_str contains all numeric scalar fields and basic JSON structure without
// requiring brittle full-string matching. This makes the test resilient to new fields being added.
TEST(ServerStatsJson, ContainsAllScalarFields) {
  ServerStats st;
  // Populate with some non-zero, distinct-ish values so textual search is unique.
  st.totalBytesQueued = 42;
  st.totalBytesWrittenImmediate = 7;
  st.totalBytesWrittenFlush = 99;
  st.deferredWriteEvents = 3;
  st.flushCycles = 5;
  st.epollModFailures = 1;
  st.maxConnectionOutboundBuffer = 1234;
#ifdef AERONET_ENABLE_OPENSSL
  st.tlsHandshakesSucceeded = 2;
  st.tlsClientCertPresent = 0;
  st.tlsAlpnStrictMismatches = 0;
  st.tlsHandshakeDurationCount = 4;
  st.tlsHandshakeDurationTotalNs = 5555;
  st.tlsHandshakeDurationMaxNs = 999;
  st.tlsAlpnDistribution.emplace_back("http/1.1", 1);
  st.tlsVersionCounts.emplace_back("TLSv1.3", 2);
  st.tlsCipherCounts.emplace_back("TLS_AES_256_GCM_SHA384", 2);
#endif

  std::string json = st.json_str();
  ASSERT_FALSE(json.empty());
  ASSERT_EQ(json.front(), '{');
  ASSERT_EQ(json.back(), '}');

  // Collect expected scalar fields & verify presence of "name":value pattern.
  st.for_each_field([&](const char* name, uint64_t value) {
    std::string needle = std::string("\"") + name + "\":" + std::to_string(value);
    EXPECT_TRUE(json.contains(needle)) << "Missing field mapping: " << needle << " in json=" << json;
  });

  // Minimal structural sanity: no trailing comma before closing brace.
  ASSERT_FALSE(json.contains(",}")) << "Trailing comma present in JSON: " << json;
}