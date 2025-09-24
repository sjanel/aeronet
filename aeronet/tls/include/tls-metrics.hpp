// aeronet - internal TLS metrics structure
// Extracted from HttpServer to allow helpers to update metrics via direct reference.
#pragma once

#include <cstdint>
#include <string>

#include "flat-hash-map.hpp"

namespace aeronet {

struct TlsMetricsInternal {
  uint64_t handshakesSucceeded{};
  uint64_t clientCertPresent{};
  uint64_t alpnStrictMismatches{};  // updated externally when strict ALPN mismatch occurs
  flat_hash_map<std::string, uint64_t> alpnDistribution;
  flat_hash_map<std::string, uint64_t> versionCounts;
  flat_hash_map<std::string, uint64_t> cipherCounts;
  uint64_t handshakeDurationTotalNs{};
  uint64_t handshakeDurationCount{};
  uint64_t handshakeDurationMaxNs{};
};

}  // namespace aeronet
