#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>

#include "aeronet/city-hash.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/single-writer-counter.hpp"

namespace aeronet {

// Updated by the server event loop, read by stats() from any thread: counters are single-writer atomics, and the maps
// (only updated on TLS handshakes) are guarded by mapsMutex.
struct TlsMetricsInternal {
  using RawChars32Uint64Map = flat_hash_map<RawChars32, uint64_t, CityHash, std::equal_to<>>;
  using SvUint64Map = flat_hash_map<std::string_view, uint64_t, CityHash, std::equal_to<>>;
  using Counter = SingleWriterCounter<uint64_t>;

  // Mutex guarding the maps below. Moves (only made while the server is stopped) start with a fresh mutex.
  struct MapsMutex {
    MapsMutex() noexcept = default;

    MapsMutex(const MapsMutex&) = delete;
    MapsMutex([[maybe_unused]] MapsMutex&& rhs) noexcept {}
    MapsMutex& operator=(const MapsMutex&) = delete;
    MapsMutex& operator=([[maybe_unused]] MapsMutex&& rhs) noexcept { return *this; }

    ~MapsMutex() = default;

    std::mutex mutex;
  };

  // Increments the count of the given key in one of the maps below.
  template <class Map, class Key>
  void incrementMapCount(Map& map, const Key& key) {
    std::scoped_lock lock(mapsMutex.mutex);
    auto [it, inserted] = map.emplace(key, 1);
    if (!inserted) {
      ++it->second;
    }
  }

  Counter handshakesSucceeded;
  Counter handshakesFull;
  Counter handshakesResumed;
  Counter handshakesFailed;

  Counter handshakesRejectedConcurrency;
  Counter handshakesRejectedRateLimit;
  Counter clientCertPresent;
  Counter alpnStrictMismatches;  // updated externally when strict ALPN mismatch occurs

  mutable MapsMutex mapsMutex;
  // Best-effort bucketing of fatal handshake failures / rejections.
  // Keys are short stable identifiers (e.g. "ssl_error", "timeout", "rate_limited").
  SvUint64Map handshakeFailureReasons;
  RawChars32Uint64Map alpnDistribution;
  RawChars32Uint64Map versionCounts;
  RawChars32Uint64Map cipherCounts;
  Counter handshakeDurationTotalNs;
  Counter handshakeDurationCount;
  Counter handshakeDurationMaxNs;
  Counter ktlsSendEnabledConnections;
  Counter ktlsSendEnableFallbacks;
  Counter ktlsSendForcedShutdowns;
  Counter ktlsSendBytes;
};

}  // namespace aeronet
