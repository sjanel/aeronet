#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/otel-config.hpp"

namespace aeronet::tracing {

// Minimal, header-only tracing facade.
// By default this is a no-op implementation. When building with
// -DAERONET_ENABLE_OPENTELEMETRY=ON and linking opentelemetry-cpp, an
// implementation can be provided that forwards to the SDK.

struct Span {
  virtual ~Span() = default;
  virtual void setAttribute([[maybe_unused]] std::string_view key, [[maybe_unused]] int64_t val) noexcept {}
  virtual void setAttribute([[maybe_unused]] std::string_view key, [[maybe_unused]] std::string_view val) noexcept {}
  virtual void end() noexcept {}
};

using SpanPtr = std::unique_ptr<Span>;

// RAII scope helper that ends span at destruction if not already ended.
struct SpanRAII {
  explicit SpanRAII(SpanPtr spanPtr) noexcept : span(std::move(spanPtr)) {}

  SpanRAII(const SpanRAII &) = delete;
  SpanRAII(SpanRAII &&) noexcept = default;
  SpanRAII &operator=(const SpanRAII &) = delete;
  SpanRAII &operator=(SpanRAII &&) noexcept = default;

  ~SpanRAII() {
    if (span) {
      span->end();
    }
  }

  SpanPtr span;
};

// Forward declaration for implementation details
class TelemetryContextImpl;

// Telemetry context - one per HttpServer instance.
// Encapsulates OpenTelemetry TracerProvider and MeterProvider.
// No global singletons - each instance is independent.
class TelemetryContext {
 public:
  TelemetryContext() noexcept;

  explicit TelemetryContext(const aeronet::OtelConfig &cfg);

  // Non-copyable, movable
  TelemetryContext(const TelemetryContext &) = delete;
  TelemetryContext &operator=(const TelemetryContext &) = delete;
  TelemetryContext(TelemetryContext &&) noexcept;
  TelemetryContext &operator=(TelemetryContext &&) noexcept;

  ~TelemetryContext();

  // Create a span with given name. Returns nullptr if tracing disabled/failed.
  SpanPtr createSpan(std::string_view name) noexcept;

  // Increment a counter by delta. No-op if metrics disabled/failed.
  void counterAdd(std::string_view name, uint64_t delta = 1UL) noexcept;

 private:
  std::unique_ptr<TelemetryContextImpl> _impl;
};

// Quick helper to know if tracing is enabled at compile-time
constexpr bool enabled() noexcept {
#ifdef AERONET_ENABLE_OPENTELEMETRY
  return true;
#else
  return false;
#endif
}

}  // namespace aeronet::tracing
