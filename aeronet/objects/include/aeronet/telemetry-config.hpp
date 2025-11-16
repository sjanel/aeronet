#pragma once

#include <cstdint>
#include <ranges>
#include <string_view>

#include "aeronet/concatenated-strings.hpp"
#include "aeronet/dogstatsd.hpp"
#include "aeronet/static-concatenated-strings.hpp"

namespace aeronet {

class TelemetryConfig {
 public:
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

  // OTLP endpoint. May be a TCP URL (http://host:4318 or host:4317 for gRPC) or a unix socket URI
  // (e.g. unix:///var/run/collector.sock) depending on exporter support.
  TelemetryConfig& withEndpoint(std::string_view ep) {
    _staticStrings.set(0, ep);
    return *this;
  }

  // Service name to attach to traces. If empty, the application may supply a default.
  TelemetryConfig& withServiceName(std::string_view serviceName) {
    _staticStrings.set(1, serviceName);
    return *this;
  }

  // DogStatsD socket path (e.g. /var/run/datadog/dsd.socket). Empty => consult environment.
  TelemetryConfig& withDogStatsdSocketPath(std::string_view path) {
    _staticStrings.set(2, path);
    return *this;
  }

  // Optional namespace prefix for DogStatsD metrics (defaults to serviceName when empty).
  TelemetryConfig& withDogStatsdNamespace(std::string_view ns) {
    _staticStrings.set(3, ns);
    return *this;
  }

  // Enable DogStatsD metrics emission via unix domain socket even when otelEnabled is disabled.
  TelemetryConfig& enableDogStatsDMetrics(bool on = true) {
    dogStatsDEnabled = on;
    return *this;
  }

  // Append an additional DogStatsD tag sent with every metric (format key:value).
  TelemetryConfig& addDogStatsdTag(std::string_view tag) {
    _dogstatsdTags.append(tag);
    return *this;
  }

  // Optional headers to send with exporter requests (e.g. API keys). Stored as pairs (name, value).
  TelemetryConfig& addHttpHeader(std::string_view name, std::string_view value);

  TelemetryConfig& withSampleRate(double sampleRate) {
    this->sampleRate = sampleRate;
    return *this;
  }

  bool operator==(const TelemetryConfig&) const noexcept = default;

  // Enable/disable Telemetry instrumentation for this server instance.
  bool otelEnabled{false};

  // Enable DogStatsD metrics emission via unix domain socket even when otelEnabled is disabled.
  // Note that if otelEnabled is true, the metric will be emitted via both OTLP and DogStatsD.
  bool dogStatsDEnabled{false};

  // Sampling ratio [0.0, 1.0]. 1.0 = sample all, 0.0 = sample none. Default: 1.0
  double sampleRate{1.0};

 private:
  // endpoint, serviceName, dogstatsdSocketPath, dogstatsdNamespace
  StaticConcatenatedStrings<4, uint32_t> _staticStrings;

  DogStatsD::DogStatsDTags _dogstatsdTags;

  SmallConcatenatedStrings _httpHeaders;
};

}  // namespace aeronet