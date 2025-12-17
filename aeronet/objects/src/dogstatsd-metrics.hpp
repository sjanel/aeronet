#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/dogstatsd.hpp"
#include "aeronet/telemetry-config.hpp"

namespace aeronet::tracing::detail {

class DogStatsdMetrics {
 public:
  DogStatsdMetrics() = default;

  explicit DogStatsdMetrics(const TelemetryConfig& cfg) {
    if (cfg.dogStatsDEnabled) {
      std::string_view metricNamespace =
          cfg.dogstatsdNamespace().empty() ? cfg.serviceName() : cfg.dogstatsdNamespace();
      _client = DogStatsD(cfg.dogstatsdSocketPath(), metricNamespace);

      _pTags = &cfg.dogstatsdTags();
    }
  }

  void increment(std::string_view metric, uint64_t delta = 1UL) const noexcept {
    if (_pTags != nullptr) {
      _client.increment(metric, delta, *_pTags);
    }
  }

  void gauge(std::string_view metric, int64_t value) const noexcept {
    if (_pTags != nullptr) {
      _client.gauge(metric, value, *_pTags);
    }
  }

  void histogram(std::string_view metric, double value) const noexcept {
    if (_pTags != nullptr) {
      _client.histogram(metric, value, *_pTags);
    }
  }

  void timing(std::string_view metric, std::chrono::milliseconds ms) const noexcept {
    if (_pTags != nullptr) {
      _client.timing(metric, ms, *_pTags);
    }
  }

  [[nodiscard]] const DogStatsD& dogstatsdClient() const noexcept { return _client; }

 private:
  DogStatsD _client;
  const DogStatsD::DogStatsDTags* _pTags{nullptr};
};

}  // namespace aeronet::tracing::detail
