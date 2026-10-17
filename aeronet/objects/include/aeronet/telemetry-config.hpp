#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <ranges>
#include <span>
#include <string_view>

#include "aeronet/city-hash.hpp"
#include "aeronet/concatenated-strings.hpp"
#include "aeronet/dogstatsd.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/static-concatenated-strings.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

class TelemetryConfig {
 public:
  using HistogramBoundariesMap = flat_hash_map<RawChars32, vector<double>, CityHash, std::equal_to<>>;

  void validate();

  // OTLP endpoint. May be a TCP URL (http://host:4318 or host:4317 for gRPC) or a unix socket URI
  // (e.g. unix:///var/run/collector.sock) depending on exporter support.
  [[nodiscard]] std::string_view endpoint() const { return _staticStrings[0]; }

  // Service name to attach to traces. If empty, the application may supply a default.
  [[nodiscard]] std::string_view serviceName() const { return _staticStrings[1]; }

  // DogStatsD socket path (e.g. /var/run/datadog/dsd.socket). Empty => disabled.
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
  TelemetryConfig& withSampleRate(double rate);

  bool operator==(const TelemetryConfig&) const noexcept = default;

  // Enable/disable Telemetry instrumentation for this server instance.
  bool otelEnabled{false};

  // Enable DogStatsD metrics emission via unix domain socket even when otelEnabled is disabled.
  // Note that if otel is also enabled, the metric will be emitted via both OTLP and DogStatsD.
  bool dogStatsDEnabled{false};

  // Sampling ratio [0.0, 1.0]. 1.0 = sample all, 0.0 = sample none. Default: 1.0
  float sampleRate{1.0F};

  // Interval between metric exports, and maximum delay before ended spans are exported (default: 10000ms).
  // Spans are exported in batches by a background thread, never by the event loop.
  std::chrono::milliseconds exportInterval{std::chrono::milliseconds{10000}};

  // Timeout for metric exports (default: 5000ms)
  std::chrono::milliseconds exportTimeout{std::chrono::milliseconds{5000}};

  // Internal storage, read and written through the methods above. Public only so that TelemetryConfig stays an
  // aggregate, whose members glaze reflects (see glaze-internal-members.hpp).

  // endpoint, serviceName, dogstatsdSocketPath, dogstatsdNamespace
  StaticConcatenatedStrings<4, uint32_t> _staticStrings;

  DogStatsD::DogStatsDTags _dogstatsdTags;

  ConcatenatedStrings32 _httpHeaders;

  HistogramBoundariesMap _histogramBuckets;
};

}  // namespace aeronet

#ifdef AERONET_ENABLE_GLAZE

#include <glaze/glaze.hpp>
#include <map>
#include <string>

#include "aeronet/glaze-chrono-durations-adapters.hpp"  // IWYU pragma: export
#include "aeronet/glaze-internal-members.hpp"
#include "aeronet/glaze-vector-sv-adapter.hpp"

// TelemetryConfig is reflected by glaze: only its internal members (see glaze-internal-members.hpp) need an entry.
template <>
struct glz::meta<aeronet::TelemetryConfig> : aeronet::glz_detail::SkipInternalMembers {
  using T = aeronet::TelemetryConfig;
  static constexpr auto modify = object(
      "endpoint", custom<[](T& self, const std::string& str) { self.withEndpoint(str); }, &T::endpoint>, "serviceName",
      custom<[](T& self, const std::string& str) { self.withServiceName(str); }, &T::serviceName>,
      "dogstatsdSocketPath",
      custom<[](T& self, const std::string& str) { self.withDogStatsdSocketPath(str); }, &T::dogstatsdSocketPath>,
      "dogstatsdNamespace",
      custom<[](T& self, const std::string& str) { self.withDogStatsdNamespace(str); }, &T::dogstatsdNamespace>,
      // Tags as array of "key:value" strings
      "dogstatsdTags", &T::_dogstatsdTags,
      // HTTP headers as array of "name:value" strings
      "httpHeaders",
      custom<[](T& self, const ::aeronet::vector<std::string>& headers) {
        for (const auto& hdr : headers) {
          if (auto sep = hdr.find(':'); sep != std::string::npos) {
            self.addHttpHeader(std::string_view(hdr).substr(0, sep), std::string_view(hdr).substr(sep + 1));
          }
        }
      },
             aeronet::glz_detail::ToStringViewVector<&T::httpHeadersRange>>,
      // Histogram buckets as JSON object {"metric_name": [1.0, 5.0, 10.0]}
      "histogramBuckets",
      custom<[](T& self, std::map<std::string, ::aeronet::vector<double>> buckets) {
        for (auto& [name, boundaries] : buckets) {
          self.addHistogramBuckets(name, boundaries);
        }
      },
             [](const T& self) {
               std::map<std::string, ::aeronet::vector<double>> result;
               for (const auto& [key, boundaries] : self.histogramBuckets()) {
                 result[std::string(std::string_view(key))] =
                     ::aeronet::vector<double>(boundaries.begin(), boundaries.end());
               }
               return result;
             }>);

  // One entry per packed string, plus the dogstatsdTags, httpHeaders and histogramBuckets lists.
  static_assert(aeronet::glz_detail::NbObjectEntries<decltype(modify)> == decltype(T::_staticStrings)::size() + 3U,
                "each internal member of TelemetryConfig needs an entry in modify");
};

#endif