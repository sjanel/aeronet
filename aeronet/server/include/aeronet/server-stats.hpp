#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef AERONET_ENABLE_OPENSSL
#include <utility>
#include <vector>
#endif

namespace aeronet {

struct ServerStats {
  // Serialize this stats snapshot to JSON (single object).
  [[nodiscard]] std::string json_str() const;

  // Introspection enumeration of scalar numeric fields (order matches serialization prefix order).
  template <class F>
  void for_each_field(F&& fun) const {
    fun("totalBytesQueued", totalBytesQueued);
    fun("totalBytesWrittenImmediate", totalBytesWrittenImmediate);
    fun("totalBytesWrittenFlush", totalBytesWrittenFlush);
    fun("deferredWriteEvents", deferredWriteEvents);
    fun("flushCycles", flushCycles);
    fun("epollModFailures", epollModFailures);
    fun("maxConnectionOutboundBuffer", static_cast<uint64_t>(maxConnectionOutboundBuffer));
    fun("totalRequestsServed", totalRequestsServed);
#ifdef AERONET_ENABLE_OPENSSL
    fun("ktlsSendEnabledConnections", ktlsSendEnabledConnections);
    fun("ktlsSendEnableFallbacks", ktlsSendEnableFallbacks);
    fun("ktlsSendForcedShutdowns", ktlsSendForcedShutdowns);
    fun("ktlsSendBytes", ktlsSendBytes);
    fun("tlsHandshakesSucceeded", tlsHandshakesSucceeded);
    fun("tlsHandshakesFull", tlsHandshakesFull);
    fun("tlsHandshakesResumed", tlsHandshakesResumed);
    fun("tlsHandshakesFailed", tlsHandshakesFailed);
    fun("tlsHandshakesRejectedConcurrency", tlsHandshakesRejectedConcurrency);
    fun("tlsHandshakesRejectedRateLimit", tlsHandshakesRejectedRateLimit);
    fun("tlsClientCertPresent", tlsClientCertPresent);
    fun("tlsAlpnStrictMismatches", tlsAlpnStrictMismatches);
    fun("tlsHandshakeDurationCount", tlsHandshakeDurationCount);
    fun("tlsHandshakeDurationTotalNs", tlsHandshakeDurationTotalNs);
    fun("tlsHandshakeDurationMaxNs", tlsHandshakeDurationMaxNs);
#endif
  }

  uint64_t totalBytesQueued{};
  uint64_t totalBytesWrittenImmediate{};
  uint64_t totalBytesWrittenFlush{};
  uint64_t deferredWriteEvents{};
  uint64_t flushCycles{};
  uint64_t epollModFailures{};
  std::size_t maxConnectionOutboundBuffer{};
  uint64_t totalRequestsServed{};
#ifdef AERONET_ENABLE_OPENSSL
  uint64_t ktlsSendEnabledConnections{};
  uint64_t ktlsSendEnableFallbacks{};
  uint64_t ktlsSendForcedShutdowns{};
  uint64_t ktlsSendBytes{};
  uint64_t tlsHandshakesSucceeded{};
  uint64_t tlsHandshakesFull{};
  uint64_t tlsHandshakesResumed{};
  uint64_t tlsHandshakesFailed{};
  uint64_t tlsHandshakesRejectedConcurrency{};
  uint64_t tlsHandshakesRejectedRateLimit{};
  uint64_t tlsClientCertPresent{};
  uint64_t tlsAlpnStrictMismatches{};
  std::vector<std::pair<std::string, uint64_t>> tlsAlpnDistribution;         // snapshot of ALPN protocol counts
  std::vector<std::pair<std::string, uint64_t>> tlsHandshakeFailureReasons;  // best-effort failure/reject bucketing
  std::vector<std::pair<std::string, uint64_t>> tlsVersionCounts;            // per TLS version counts
  std::vector<std::pair<std::string, uint64_t>> tlsCipherCounts;             // per cipher counts
  uint64_t tlsHandshakeDurationCount{};
  uint64_t tlsHandshakeDurationTotalNs{};
  uint64_t tlsHandshakeDurationMaxNs{};
#endif
};

}  // namespace aeronet
