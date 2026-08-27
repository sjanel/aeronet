#pragma once

#include <opentelemetry/common/key_value_iterable.h>
#include <opentelemetry/nostd/function_ref.h>
#include <opentelemetry/nostd/string_view.h>

#include <algorithm>
#include <cstddef>

#include "aeronet/metric-label.hpp"

namespace aeronet::tracing::detail {

class OtelMetricLabels final : public opentelemetry::common::KeyValueIterable {
 public:
  explicit OtelMetricLabels(MetricLabels labels) noexcept : _labels(labels) {}

  [[nodiscard]] bool ForEachKeyValue(
      opentelemetry::nostd::function_ref<bool(opentelemetry::nostd::string_view, opentelemetry::common::AttributeValue)>
          callback) const noexcept override {
    return std::ranges::all_of(_labels, [&callback](const auto& label) {
      return callback(opentelemetry::nostd::string_view(label.key.data(), label.key.size()),
                      opentelemetry::nostd::string_view(label.value.data(), label.value.size()));
    });
  }

  [[nodiscard]] std::size_t size() const noexcept override { return _labels.size(); }

 private:
  MetricLabels _labels;
};

}  // namespace aeronet::tracing::detail
