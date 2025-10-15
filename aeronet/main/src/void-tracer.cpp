#include <cstdint>
#include <string_view>

#include "aeronet/otel-config.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "invalid_argument_exception.hpp"

namespace aeronet::tracing {

// No-op implementation when OpenTelemetry is disabled
class TelemetryContextImpl {};

TelemetryContext::TelemetryContext() noexcept = default;

TelemetryContext::TelemetryContext(const OtelConfig& cfg) {
  if (cfg.enabled) {
    throw invalid_argument("Unable to create TelemetryContext - aeronet has been compiled without Otel support");
  }
}

TelemetryContext::~TelemetryContext() = default;
TelemetryContext::TelemetryContext(TelemetryContext&&) noexcept = default;
TelemetryContext& TelemetryContext::operator=(TelemetryContext&&) noexcept = default;

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
SpanPtr TelemetryContext::createSpan([[maybe_unused]] std::string_view name) noexcept { return nullptr; }
void TelemetryContext::counterAdd([[maybe_unused]] std::string_view name, [[maybe_unused]] uint64_t delta) noexcept {}

}  // namespace aeronet::tracing