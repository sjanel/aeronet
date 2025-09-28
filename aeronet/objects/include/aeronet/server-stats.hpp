#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
    fun("streamingChunkCoalesced", streamingChunkCoalesced);
    fun("streamingChunkLarge", streamingChunkLarge);
    fun("maxConnectionOutboundBuffer", static_cast<uint64_t>(maxConnectionOutboundBuffer));
#ifdef AERONET_ENABLE_OPENSSL
    fun("tlsHandshakesSucceeded", tlsHandshakesSucceeded);
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
  uint64_t streamingChunkCoalesced{};  // number of chunked streaming emits that used coalesced buffer path
  uint64_t streamingChunkLarge{};      // number of chunked streaming emits that used large multi-enqueue path
  std::size_t maxConnectionOutboundBuffer{};
#ifdef AERONET_ENABLE_OPENSSL
  uint64_t tlsHandshakesSucceeded{};
  uint64_t tlsClientCertPresent{};
  uint64_t tlsAlpnStrictMismatches{};
  std::vector<std::pair<std::string, uint64_t>> tlsAlpnDistribution;  // snapshot of ALPN protocol counts
  std::vector<std::pair<std::string, uint64_t>> tlsVersionCounts;     // per TLS version counts
  std::vector<std::pair<std::string, uint64_t>> tlsCipherCounts;      // per cipher counts
  uint64_t tlsHandshakeDurationCount{};
  uint64_t tlsHandshakeDurationTotalNs{};
  uint64_t tlsHandshakeDurationMaxNs{};
#endif
};

}  // namespace aeronet
