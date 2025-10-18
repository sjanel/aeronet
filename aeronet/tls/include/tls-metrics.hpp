#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "flat-hash-map.hpp"

namespace aeronet {

struct TlsMetricsInternal {
  using StringUint64Map = flat_hash_map<std::string, uint64_t, std::hash<std::string_view>, std::equal_to<>>;

  uint64_t handshakesSucceeded{};
  uint64_t clientCertPresent{};
  uint64_t alpnStrictMismatches{};  // updated externally when strict ALPN mismatch occurs
  StringUint64Map alpnDistribution;
  StringUint64Map versionCounts;
  StringUint64Map cipherCounts;
  uint64_t handshakeDurationTotalNs{};
  uint64_t handshakeDurationCount{};
  uint64_t handshakeDurationMaxNs{};
};

}  // namespace aeronet
