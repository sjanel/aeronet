#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/metric-label.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::tracing::detail {

// Terminates the name, each label key and each label value of a metric key (no exporter accepts it in them), so that
// the name is a C string too: the OpenTelemetry SDK before 1.30 reads instrument names as such.
inline constexpr char kMetricKeySep = '\0';

// Writes in key the identifier of the time series of a metric - its name and labels, each followed by kMetricKeySep -
// and returns it.
inline std::string_view BuildMetricKey(std::string_view name, MetricLabels labels, RawChars32& key) {
  std::size_t keySize = name.size() + 1U;
  for (const auto& [labelKey, labelValue] : labels) {
    keySize += labelKey.size() + labelValue.size() + 2U;
  }
  key.reserve(keySize);
  char* pKey = Append(name, key.data());
  *pKey++ = kMetricKeySep;
  for (const auto& [labelKey, labelValue] : labels) {
    pKey = Append(labelKey, pKey);
    *pKey++ = kMetricKeySep;
    pKey = Append(labelValue, pKey);
    *pKey++ = kMetricKeySep;
  }
  key.setEnd(pKey);
  return key;
}

}  // namespace aeronet::tracing::detail
