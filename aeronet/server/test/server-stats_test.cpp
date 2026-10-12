#include "aeronet/server-stats.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace aeronet {

TEST(ServerStatsTest, JsonAndFieldsIncludeZerocopyCounters) {
  ServerStats stats;
  stats.zerocopySends = 21;
  stats.zerocopyBytes = 22;
  stats.zerocopyCopiedSends = 23;
  stats.zerocopyNoBufferSpaceFallbacks = 24;
  stats.zerocopyRetainLimitFallbacks = 25;
  stats.zerocopyResetConnections = 26;

  const std::string json = stats.json_str();
  EXPECT_TRUE(json.contains("\"zerocopySends\":21"));
  EXPECT_TRUE(json.contains("\"zerocopyBytes\":22"));
  EXPECT_TRUE(json.contains("\"zerocopyCopiedSends\":23"));
  EXPECT_TRUE(json.contains("\"zerocopyNoBufferSpaceFallbacks\":24"));
  EXPECT_TRUE(json.contains("\"zerocopyRetainLimitFallbacks\":25"));
  EXPECT_TRUE(json.contains("\"zerocopyResetConnections\":26"));
  EXPECT_TRUE(json.starts_with('{'));
  EXPECT_TRUE(json.ends_with('}'));

  uint64_t zerocopySum = 0;
  stats.for_each_field([&zerocopySum](std::string_view name, uint64_t value) {
    if (name.starts_with("zerocopy")) {
      zerocopySum += value;
    }
  });
  EXPECT_EQ(zerocopySum, 21U + 22U + 23U + 24U + 25U + 26U);
}

#ifdef AERONET_ENABLE_OPENSSL
TEST(ServerStatsTest, JsonIncludesKtlsFieldsWhenOpenSslEnabled) {
  ServerStats stats;
  stats.totalBytesQueued = 1;
  stats.totalBytesWrittenImmediate = 2;
  stats.totalBytesWrittenFlush = 3;
  stats.deferredWriteEvents = 4;
  stats.flushCycles = 5;
  stats.epollModFailures = 6;
  stats.maxConnectionOutboundBuffer = 7;
  stats.totalRequestsServed = 8;
  stats.ktlsSendEnabledConnections = 9;
  stats.ktlsSendEnableFallbacks = 10;
  stats.ktlsSendForcedShutdowns = 11;
  stats.ktlsSendBytes = 12;
  stats.tlsHandshakesSucceeded = 13;
  stats.tlsClientCertPresent = 14;
  stats.tlsAlpnStrictMismatches = 15;

  stats.tlsAlpnDistribution.emplace_back("http/1.1", 2);
  stats.tlsAlpnDistribution.emplace_back("h2", 3);

  stats.tlsVersionCounts.emplace_back("TLS1.3", 3);
  stats.tlsVersionCounts.emplace_back("TLS1.2", 5);

  stats.tlsCipherCounts.emplace_back("TLS_AES_256_GCM_SHA384", 4);
  stats.tlsCipherCounts.emplace_back("TLS_CHACHA20_POLY1305_SHA256", 6);

  stats.tlsHandshakeFailureReasons.emplace_back("bad_certificate", 7);
  stats.tlsHandshakeFailureReasons.emplace_back("unsupported_protocol", 8);

  stats.tlsHandshakeDurationCount = 16;
  stats.tlsHandshakeDurationTotalNs = 17;
  stats.tlsHandshakeDurationMaxNs = 18;

  const std::string json = stats.json_str();

  EXPECT_TRUE(json.contains("\"ktlsSendEnabledConnections\":9"));
  EXPECT_TRUE(json.contains("\"ktlsSendEnableFallbacks\":10"));
  EXPECT_TRUE(json.contains("\"ktlsSendForcedShutdowns\":11"));
  EXPECT_TRUE(json.contains("\"ktlsSendBytes\":12"));
  EXPECT_TRUE(json.contains("\"tlsAlpnDistribution\":[{"));
  EXPECT_TRUE(json.contains("\"tlsVersionCounts\":[{"));
  EXPECT_TRUE(json.contains("\"tlsCipherCounts\":[{"));
  EXPECT_TRUE(json.contains("TLS_AES_256_GCM_SHA384"));
  EXPECT_TRUE(json.contains("TLS_CHACHA20_POLY1305_SHA256"));

  EXPECT_EQ(std::ranges::count(json, '{'), std::ranges::count(json, '}'));
  EXPECT_EQ(std::ranges::count(json, '['), std::ranges::count(json, ']'));
  EXPECT_TRUE(json.starts_with('{'));
  EXPECT_TRUE(json.ends_with('}'));
}
#endif

}  // namespace aeronet
