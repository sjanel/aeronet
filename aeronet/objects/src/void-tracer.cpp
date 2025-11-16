#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "aeronet/dogstatsd.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "dogstatsd-metrics.hpp"

namespace aeronet::tracing {

// No-op implementation when OpenTelemetry is disabled, but still allows DogStatsD metrics.
class TelemetryContextImpl {
 public:
  explicit TelemetryContextImpl(const aeronet::TelemetryConfig& cfg) : dogstatsd(cfg) {}

  detail::DogStatsdMetrics dogstatsd;
};

TelemetryContext::TelemetryContext() noexcept = default;

TelemetryContext::TelemetryContext(const TelemetryConfig& cfg) {
  if (cfg.otelEnabled) {
    throw std::invalid_argument("Unable to create TelemetryContext - aeronet has been compiled without Otel support");
  }
  if (cfg.dogStatsDEnabled) {
    _impl = std::make_unique<TelemetryContextImpl>(cfg);
  }
}

TelemetryContext::~TelemetryContext() = default;
TelemetryContext::TelemetryContext(TelemetryContext&&) noexcept = default;
TelemetryContext& TelemetryContext::operator=(TelemetryContext&&) noexcept = default;

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
SpanPtr TelemetryContext::createSpan([[maybe_unused]] std::string_view name) const noexcept { return nullptr; }

void TelemetryContext::counterAdd(std::string_view name, uint64_t delta) const noexcept {
  if (_impl) {
    _impl->dogstatsd.increment(name, delta);
  }
}

const DogStatsD* TelemetryContext::dogstatsdClient() const noexcept {
  if (_impl) {
    return &_impl->dogstatsd.dogstatsdClient();
  }
  return nullptr;
}

}  // namespace aeronet::tracing