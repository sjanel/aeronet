#include "aeronet/telemetry-config.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <span>
#include <stdexcept>
#include <string_view>

#include "aeronet/http-header.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet {

void TelemetryConfig::validate() {
  if (dogStatsDEnabled) {
    if (dogstatsdSocketPath().empty()) {
      if (const char* env = std::getenv("DD_DOGSTATSD_SOCKET")) {
        withDogStatsdSocketPath(env);
      }
      if (const char* env = std::getenv("DD_DOGSTATSD_SOCKET_PATH")) {
        withDogStatsdSocketPath(env);
      }
    }
    if (dogstatsdSocketPath().empty()) {
      throw std::invalid_argument("DogStatsD metrics enabled but no socket path configured");
    }
  }
  if (!otelEnabled) {
    return;
  }
  if (sampleRate < 0.0F || sampleRate > 1.0F) {
    log::critical("Invalid sample rate {}, must be between 0 and 1", sampleRate);
    throw std::invalid_argument("Invalid sample rate");
  }
  if (exportTimeout >= exportInterval) {
    throw std::invalid_argument(
        std::format("Export timeout {}ms must be less than export interval {}ms",
                    std::chrono::duration_cast<std::chrono::milliseconds>(exportTimeout).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(exportInterval).count()));
  }

  auto svcName = serviceName();
  if (!svcName.empty()) {
    static constexpr std::string_view kServiceTagPrefix = "service:";
    RawChars serviceTag(kServiceTagPrefix.size() + svcName.size());
    serviceTag.unchecked_append(kServiceTagPrefix);
    serviceTag.unchecked_append(svcName);
    auto tags = dogstatsdTagsRange();
    if (std::ranges::find(tags, serviceTag) == tags.end()) {
      _dogstatsdTags.append(serviceTag);
    }
  }
}

TelemetryConfig& TelemetryConfig::withEndpoint(std::string_view ep) {
  _staticStrings.set(0, ep);
  return *this;
}

TelemetryConfig& TelemetryConfig::withServiceName(std::string_view serviceName) {
  _staticStrings.set(1, serviceName);
  return *this;
}

TelemetryConfig& TelemetryConfig::withDogStatsdSocketPath(std::string_view path) {
  _staticStrings.set(2, path);
  return *this;
}

TelemetryConfig& TelemetryConfig::withDogStatsdNamespace(std::string_view ns) {
  _staticStrings.set(3, ns);
  return *this;
}

TelemetryConfig& TelemetryConfig::enableDogStatsDMetrics(bool on) {
  dogStatsDEnabled = on;
  return *this;
}

TelemetryConfig& TelemetryConfig::addDogStatsdTag(std::string_view tag) {
  _dogstatsdTags.append(tag);
  return *this;
}

TelemetryConfig& TelemetryConfig::withSampleRate(double sampleRate) {
  this->sampleRate = static_cast<float>(sampleRate);
  return *this;
}

TelemetryConfig& TelemetryConfig::addHistogramBuckets(std::string_view name, std::span<const double> boundaries) {
  if (name.empty()) {
    throw std::invalid_argument("Histogram bucket instrument name cannot be empty");
  }
  if (boundaries.size() < 2UL) {
    throw std::invalid_argument(
        std::format("Histogram '{}' bucket boundaries must contain at least two elements", std::string_view(name)));
  }
  if (!std::ranges::all_of(boundaries, [](double val) { return std::isfinite(val); })) {
    throw std::invalid_argument(std::format("Histogram '{}' has non-finite bucket boundary", std::string_view(name)));
  }
  if (std::ranges::adjacent_find(boundaries, [](double lhs, double rhs) { return lhs >= rhs; }) != boundaries.end()) {
    throw std::invalid_argument(
        std::format("Histogram '{}' bucket boundaries not strictly increasing", std::string_view(name)));
  }
  const auto [it, inserted] =
      _histogramBuckets.emplace(RawChars32{name}, vector<double>{boundaries.begin(), boundaries.end()});
  if (!inserted) {
    log::warn("Overwriting '{}' histogram bucket boundaries", name);
    it->second.assign_range(boundaries);
  }
  return *this;
}

TelemetryConfig& TelemetryConfig::addHttpHeader(std::string_view name, std::string_view value) {
  name = TrimOws(name);
  if (!http::IsValidHeaderName(name)) {
    throw std::invalid_argument("HTTP header name is invalid");
  }
  value = TrimOws(value);
  if (!http::IsValidHeaderValue(value)) {
    throw std::invalid_argument("HTTP header value is invalid");
  }

  RawChars header(name.size() + 1U + value.size());
  header.unchecked_append(name);
  header.unchecked_push_back(':');
  header.unchecked_append(value);
  _httpHeaders.append(header);
  return *this;
}

}  // namespace aeronet