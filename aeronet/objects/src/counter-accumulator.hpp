#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <string_view>
#include <utility>

#include "aeronet/city-hash.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/log-noexcept.hpp"
#include "aeronet/metric-label.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"
#include "metric-key.hpp"

namespace aeronet::tracing::detail {

// Sums in memory the counter increments recorded between two flushes, per metric name and labels. A counter updated on
// the hot path (per socket read, per HTTP/2 frame) then costs a hash map update instead of an exporter call, which for
// DogStatsD formats a metric line.
class CounterAccumulator {
 public:
  // Adds delta to the counter of this name and labels.
  void add(std::string_view name, uint64_t delta, MetricLabels labels) noexcept {
    try {
      // Room for the labels of any counter, so that flush() decodes them without allocating.
      _labels.reserve(static_cast<decltype(_labels)::size_type>(labels.size()));
      _counters.emplace(BuildMetricKey(name, labels, _keyScratch), uint64_t{0}).first->second += delta;
    } catch (const std::exception& ex) {
      log_noexcept::error("Unable to record counter '{}': {}", name, ex.what());
    }
  }

  // Calls fn(name, delta, labels) for each counter incremented since the last flush, and resets it. The counters stay
  // in memory: incrementing them again does not allocate.
  template <class Fn>
  void flush(Fn fn) noexcept {
    for (auto& [key, delta] : _counters) {
      if (delta != 0) {
        const std::string_view name = decodeKey(key);
        fn(name, std::exchange(delta, 0), MetricLabels(_labels.data(), _labels.size()));
      }
    }
  }

 private:
  // Returns the name of the counter key, and stores its labels in _labels.
  // NOLINTNEXTLINE(bugprone-exception-escape): _labels has the room for any labels, reserved by add()
  std::string_view decodeKey(std::string_view key) noexcept {
    _labels.clear();
    const std::string_view name = PopPiece(key);
    while (!key.empty()) {
      const std::string_view labelKey = PopPiece(key);
      _labels.push_back(MetricLabel{labelKey, PopPiece(key)});
    }
    return name;
  }

  // Removes the first piece of the key, with its terminating kMetricKeySep, and returns it. Only an empty key has no
  // kMetricKeySep: it stays empty (npos + 1 wraps to 0).
  static std::string_view PopPiece(std::string_view& key) noexcept {
    const auto sepPos = key.find(kMetricKeySep);
    const std::string_view piece = key.substr(0, sepPos);
    key.remove_prefix(sepPos + 1U);
    return piece;
  }

  flat_hash_map<RawChars32, uint64_t, CityHash, std::equal_to<>> _counters;
  RawChars32 _keyScratch;
  vector<MetricLabel> _labels;
};

}  // namespace aeronet::tracing::detail
