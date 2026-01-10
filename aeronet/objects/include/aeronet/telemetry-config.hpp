#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <ranges>
#include <span>
#include <string_view>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/dogstatsd.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/static-concatenated-strings.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

class TelemetryConfig {
 public:
  using HistogramBoundariesMap =
      flat_hash_map<RawChars32, vector<double>, std::hash<std::string_view>, std::equal_to<>>;

  void validate();

  // OTLP endpoint. May be a TCP URL (http://host:4318 or host:4317 for gRPC) or a unix socket URI
  // (e.g. unix:///var/run/collector.sock) depending on exporter support.
  [[nodiscard]] std::string_view endpoint() const { return _staticStrings[0]; }

  // Service name to attach to traces. If empty, the application may supply a default.
  [[nodiscard]] std::string_view serviceName() const { return _staticStrings[1]; }

  // DogStatsD socket path (e.g. /var/run/datadog/dsd.socket). Empty => consult environment.
  [[nodiscard]] std::string_view dogstatsdSocketPath() const { return _staticStrings[2]; }

  // Optional namespace prefix for DogStatsD metrics (defaults to serviceName when empty).
  [[nodiscard]] std::string_view dogstatsdNamespace() const { return _staticStrings[3]; }

  [[nodiscard]] const DogStatsD::DogStatsDTags& dogstatsdTags() const { return _dogstatsdTags; }

  // Tags to attach to every DogStatsD metric.
  [[nodiscard]] auto dogstatsdTagsRange() const {
    return std::ranges::subrange(_dogstatsdTags.begin(), _dogstatsdTags.end());
  }

  // Optional headers to send with exporter requests (e.g. API keys). Stored as 'key: value' separated pairs.
  [[nodiscard]] auto httpHeadersRange() const {
    return std::ranges::subrange(_httpHeaders.begin(), _httpHeaders.end());
  }

  // Histogram bucket boundaries configuration.
  // Key: instrument name passed to TelemetryContext::histogram().
  // Value: strictly increasing explicit bucket boundaries (OpenTelemetry explicit-bucket histogram).
  [[nodiscard]] const HistogramBoundariesMap& histogramBuckets() const noexcept { return _histogramBuckets; }

  // OTLP endpoint. May be a TCP URL (http://host:4318 or host:4317 for gRPC) or a unix socket URI
  // (e.g. unix:///var/run/collector.sock) depending on exporter support.
  TelemetryConfig& withEndpoint(std::string_view ep);

  // Service name to attach to traces. If empty, the application may supply a default.
  TelemetryConfig& withServiceName(std::string_view serviceName);

  // DogStatsD socket path (e.g. /var/run/datadog/dsd.socket). Empty => consult environment.
  TelemetryConfig& withDogStatsdSocketPath(std::string_view path);

  // Optional namespace prefix for DogStatsD metrics (defaults to serviceName when empty).
  TelemetryConfig& withDogStatsdNamespace(std::string_view ns);

  // Enable DogStatsD metrics emission via unix domain socket even when otelEnabled is disabled.
  TelemetryConfig& enableDogStatsDMetrics(bool on = true);

  // Append an additional DogStatsD tag sent with every metric (format key:value).
  TelemetryConfig& addDogStatsdTag(std::string_view tag);

  // Optional headers to send with exporter requests (e.g. API keys). Stored as pairs (name, value).
  TelemetryConfig& addHttpHeader(std::string_view name, std::string_view value);

  // Register explicit bucket boundaries for a histogram instrument.
  // The provided boundaries must respect the following conditions:
  // - at least two boundaries must be provided
  // - all boundaries must be finite numbers (no NaN or +/-inf)
  // - boundaries must be strictly increasing
  // Note that they will be ignored by the DogStatsD exporter (you will need to configure the agent instead).
  TelemetryConfig& addHistogramBuckets(std::string_view name, std::span<const double> boundaries);

  // Configures the sampling rate [0.0, 1.0]. 1.0 = sample all, 0.0 = sample none. Default: 1.0
  TelemetryConfig& withSampleRate(double sampleRate);

  bool operator==(const TelemetryConfig&) const noexcept = default;

  // Enable/disable Telemetry instrumentation for this server instance.
  bool otelEnabled{false};

  // Enable DogStatsD metrics emission via unix domain socket even when otelEnabled is disabled.
  // Note that if otel is also enabled, the metric will be emitted via both OTLP and DogStatsD.
  bool dogStatsDEnabled{false};

  // Sampling ratio [0.0, 1.0]. 1.0 = sample all, 0.0 = sample none. Default: 1.0
  float sampleRate{1.0F};

  // Interval between metric exports (default: 10000ms)
  std::chrono::milliseconds exportInterval{std::chrono::milliseconds{10000}};

  // Timeout for metric exports (default: 5000ms)
  std::chrono::milliseconds exportTimeout{std::chrono::milliseconds{5000}};

 private:
  // endpoint, serviceName, dogstatsdSocketPath, dogstatsdNamespace
  StaticConcatenatedStrings<4, uint32_t> _staticStrings;

  DogStatsD::DogStatsDTags _dogstatsdTags;

  ConcatenatedStrings32 _httpHeaders;

  HistogramBoundariesMap _histogramBuckets;
};

}  // namespace aeronet