#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

#include "aeronet/city-hash.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/metric-label.hpp"
#include "aeronet/raw-chars.hpp"
#include "metric-key.hpp"

namespace aeronet::tracing::detail {

// The OpenTelemetry ABI v1 has no synchronous gauge: a gauge is exported there as an up-down counter, whose cumulative
// sum is the last value of the gauge when each value adds its difference with the previous one. Remembers the last
// value of each gauge, per metric name and labels (in their order).
class GaugeDeltas {
 public:
  // Returns the difference between value and the previous value of the gauge of this name and labels (value itself for
  // its first value), and remembers value as its last one.
  int64_t update(std::string_view name, MetricLabels labels, int64_t value) {
    int64_t& lastValue = _lastValues.emplace(BuildMetricKey(name, labels, _keyScratch), int64_t{0}).first->second;
    return value - std::exchange(lastValue, value);
  }

 private:
  flat_hash_map<RawChars32, int64_t, CityHash, std::equal_to<>> _lastValues;
  RawChars32 _keyScratch;
};

}  // namespace aeronet::tracing::detail
