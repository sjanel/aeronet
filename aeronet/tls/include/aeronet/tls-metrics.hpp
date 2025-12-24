#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "aeronet/flat-hash-map.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

struct TlsMetricsInternal {
  using RawChars32Uint64Map = flat_hash_map<RawChars32, uint64_t, std::hash<std::string_view>, std::equal_to<>>;
  using SvUint64Map = flat_hash_map<std::string_view, uint64_t, std::hash<std::string_view>, std::equal_to<>>;

  uint64_t handshakesSucceeded{};
  uint64_t handshakesFull{};
  uint64_t handshakesResumed{};
  uint64_t handshakesFailed{};

  uint64_t handshakesRejectedConcurrency{};
  uint64_t handshakesRejectedRateLimit{};
  uint64_t clientCertPresent{};
  uint64_t alpnStrictMismatches{};  // updated externally when strict ALPN mismatch occurs

  // Best-effort bucketing of fatal handshake failures / rejections.
  // Keys are short stable identifiers (e.g. "ssl_error", "timeout", "rate_limited").
  SvUint64Map handshakeFailureReasons;
  RawChars32Uint64Map alpnDistribution;
  RawChars32Uint64Map versionCounts;
  RawChars32Uint64Map cipherCounts;
  uint64_t handshakeDurationTotalNs{};
  uint64_t handshakeDurationCount{};
  uint64_t handshakeDurationMaxNs{};
#ifdef AERONET_ENABLE_KTLS
  uint64_t ktlsSendEnabledConnections{0};
  uint64_t ktlsSendEnableFallbacks{0};
  uint64_t ktlsSendForcedShutdowns{0};
  uint64_t ktlsSendBytes{0};
#endif
};

}  // namespace aeronet
