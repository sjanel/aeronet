#pragma once

#include <span>
#include <string_view>

namespace aeronet {

/// A non-owning key/value label attached to one metric measurement.
struct MetricLabel {
  std::string_view key;
  std::string_view value;
};

/// A non-owning view of metric labels. The labels only need to live for the duration of the metric call.
using MetricLabels = std::span<const MetricLabel>;

}  // namespace aeronet
