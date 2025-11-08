#pragma once

#include <string>
#include <utility>
#include <vector>

#include "http-header.hpp"

namespace aeronet {

struct OtelConfig {
  void validate() const;

  OtelConfig& withEndpoint(std::string ep) {
    endpoint = std::move(ep);
    return *this;
  }

  OtelConfig& addHeader(std::string name, std::string value) {
    headers.emplace_back(std::move(name), std::move(value));
    return *this;
  }

  OtelConfig& withServiceName(std::string serviceName) {
    this->serviceName = std::move(serviceName);
    return *this;
  }

  OtelConfig& withSampleRate(double sampleRate) {
    this->sampleRate = sampleRate;
    return *this;
  }

  bool operator==(const OtelConfig&) const noexcept = default;

  // Enable/disable OpenTelemetry instrumentation for this server instance.
  bool enabled{false};

  // OTLP endpoint. May be a TCP URL (http://host:4318 or host:4317 for gRPC) or a unix socket URI
  // (e.g. unix:///var/run/collector.sock) depending on exporter support.
  std::string endpoint;

  // Optional headers to send with exporter requests (e.g. API keys). Stored as pairs (name, value).
  std::vector<http::Header> headers;

  // Service name to attach to traces. If empty, the application may supply a default.
  std::string serviceName;

  // Sampling ratio [0.0, 1.0]. 1.0 = sample all, 0.0 = sample none. Default: 1.0
  double sampleRate{1.0};
};

}  // namespace aeronet