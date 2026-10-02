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

      // Copied rather than referenced: the owner of the telemetry context (HttpClient, SingleHttpServer) may be moved,
      // giving its configuration a new address.
      _tags = cfg.dogstatsdTags();
    }
  }

  void increment(std::string_view metric, uint64_t delta = 1UL) noexcept {
    if (_client.enabled()) {
      _client.increment(metric, delta, _tags);
    }
  }

  void increment(std::string_view metric, uint64_t delta, MetricLabels labels) noexcept {
    if (_client.enabled()) {
      _client.increment(metric, delta, _tags, labels);
    }
  }

  void gauge(std::string_view metric, int64_t value) noexcept {
    if (_client.enabled()) {
      _client.gauge(metric, value, _tags);
    }
  }

  void gauge(std::string_view metric, int64_t value, MetricLabels labels) noexcept {
    if (_client.enabled()) {
      _client.gauge(metric, value, _tags, labels);
    }
  }

  void histogram(std::string_view metric, double value) noexcept {
    if (_client.enabled()) {
      _client.histogram(metric, value, _tags);
    }
  }

  void histogram(std::string_view metric, double value, MetricLabels labels) noexcept {
    if (_client.enabled()) {
      _client.histogram(metric, value, _tags, labels);
    }
  }

  void timing(std::string_view metric, std::chrono::milliseconds ms) noexcept {
    if (_client.enabled()) {
      _client.timing(metric, ms, _tags);
    }
  }

  void timing(std::string_view metric, std::chrono::milliseconds ms, MetricLabels labels) noexcept {
    if (_client.enabled()) {
      _client.timing(metric, ms, _tags, labels);
    }
  }

  [[nodiscard]] DogStatsD& dogstatsdClient() noexcept { return _client; }

 private:
  DogStatsD _client;
  DogStatsD::DogStatsDTags _tags;
};

}  // namespace aeronet::tracing::detail
