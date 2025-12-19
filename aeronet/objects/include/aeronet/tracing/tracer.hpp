#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/dogstatsd.hpp"
#include "aeronet/telemetry-config.hpp"

namespace aeronet::tracing {

// Minimal, header-only tracing facade.
// By default this is a no-op implementation. When building with
// -DAERONET_ENABLE_OPENTELEMETRY=ON and linking opentelemetry-cpp, an
// implementation can be provided that forwards to the SDK.

struct Span {
  virtual ~Span() = default;

  virtual void setAttribute([[maybe_unused]] std::string_view key, [[maybe_unused]] int64_t val) noexcept = 0;

  virtual void setAttribute([[maybe_unused]] std::string_view key, [[maybe_unused]] std::string_view val) noexcept = 0;

  virtual void end() noexcept = 0;
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

// Telemetry context - one per SingleHttpServer instance.
// Encapsulates OpenTelemetry TracerProvider and MeterProvider.
// No global singletons - each instance is independent.
class TelemetryContext {
 public:
  TelemetryContext() noexcept;

  explicit TelemetryContext(const aeronet::TelemetryConfig &cfg);

  // Non-copyable, movable
  TelemetryContext(const TelemetryContext &) = delete;
  TelemetryContext &operator=(const TelemetryContext &) = delete;
  TelemetryContext(TelemetryContext &&) noexcept;
  TelemetryContext &operator=(TelemetryContext &&) noexcept;

  ~TelemetryContext();

  // Create a span with given name. Returns nullptr if tracing disabled/failed.
  [[nodiscard]] SpanPtr createSpan(std::string_view name) const noexcept;

  // Increment a counter by delta. No-op if metrics disabled/failed.
  void counterAdd(std::string_view name, uint64_t delta = 1UL) const noexcept;

  // Record a gauge value. No-op if metrics disabled/failed.
  void gauge(std::string_view name, int64_t value) const noexcept;

  // Record a histogram value. No-op if metrics disabled/failed.
  void histogram(std::string_view name, double value) const noexcept;

  // Record a timing value. No-op if metrics disabled/failed.
  void timing(std::string_view name, std::chrono::milliseconds ms) const noexcept;

  // Access underlying DogStatsD client, or nullptr if not enabled.
  // You can use it to emit custom DogStatsD metrics.
  [[nodiscard]] const DogStatsD *dogstatsdClient() const noexcept;

 private:
  std::unique_ptr<TelemetryContextImpl> _impl;
};

}  // namespace aeronet::tracing
