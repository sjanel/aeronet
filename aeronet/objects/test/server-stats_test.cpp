#include "aeronet/server-stats.hpp"

#include <gtest/gtest.h>

#include <string>

namespace aeronet {

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
  stats.tlsVersionCounts.emplace_back("TLS1.3", 3);
  stats.tlsCipherCounts.emplace_back("TLS_AES_256_GCM_SHA384", 4);
  stats.tlsHandshakeDurationCount = 16;
  stats.tlsHandshakeDurationTotalNs = 17;
  stats.tlsHandshakeDurationMaxNs = 18;

  const std::string json = stats.json_str();
  EXPECT_NE(json.find("\"ktlsSendEnabledConnections\":9"), std::string::npos);
  EXPECT_NE(json.find("\"ktlsSendEnableFallbacks\":10"), std::string::npos);
  EXPECT_NE(json.find("\"ktlsSendForcedShutdowns\":11"), std::string::npos);
  EXPECT_NE(json.find("\"ktlsSendBytes\":12"), std::string::npos);
  EXPECT_NE(json.find("\"tlsAlpnDistribution\":[{"), std::string::npos);
  EXPECT_NE(json.find("\"tlsVersionCounts\":[{"), std::string::npos);
  EXPECT_NE(json.find("\"tlsCipherCounts\":[{"), std::string::npos);
}
#endif

}  // namespace aeronet
