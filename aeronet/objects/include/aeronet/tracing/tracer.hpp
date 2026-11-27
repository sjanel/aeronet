#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/chrono-lite.hpp"
#include "aeronet/dogstatsd.hpp"
#include "aeronet/metric-label.hpp"
#include "aeronet/telemetry-config.hpp"

namespace aeronet::tracing {

// Minimal tracing facade.
// Without -D AERONET_ENABLE_OPENTELEMETRY=ON, TelemetryContext makes no span. With it, aeronet records the spans itself
// and exports them in OTLP to the configured endpoint.

// A span, made by TelemetryContext::createSpan() and owned by a SpanPtr, which recycles it instead of deleting it.
struct Span {
  virtual ~Span() = default;

  virtual void setAttribute([[maybe_unused]] std::string_view key, [[maybe_unused]] int64_t val) noexcept = 0;

  virtual void setAttribute([[maybe_unused]] std::string_view key, [[maybe_unused]] std::string_view val) noexcept = 0;

  // Ends the span. Later calls of end() and setAttribute() are ignored.
  virtual void end() noexcept = 0;

  // Called by SpanPtr instead of delete: releases the span, which the implementation may reuse (a span of
  // TelemetryContext returns to its pool, ended first if it was not). An implementation of its own may simply
  // 'delete this'.
  virtual void recycle() noexcept = 0;
};

// The deleter of SpanPtr: an empty type, a SpanPtr stays the size of a pointer.
struct SpanRecycler {
  void operator()(Span* pSpan) const noexcept { pSpan->recycle(); }
};

using SpanPtr = std::unique_ptr<Span, SpanRecycler>;

// RAII scope helper that ends span at destruction if not already ended.
struct SpanRAII {
  explicit SpanRAII(SpanPtr spanPtr) noexcept : span(std::move(spanPtr)) {}

  SpanRAII(const SpanRAII&) = delete;
  SpanRAII(SpanRAII&&) noexcept = default;
  SpanRAII& operator=(const SpanRAII&) = delete;
  SpanRAII& operator=(SpanRAII&&) noexcept = default;

  ~SpanRAII() {
    if (span) {
      span->end();
    }
  }

  SpanPtr span;
};

// Forward declaration for implementation details
class TelemetryContextImpl;

// Telemetry context - one per thread (client / SingleHttpServer) instance.
// Encapsulates the span recorder and its exporter thread, the OpenTelemetry MeterProvider and DogStatsD.
// No global singletons - each instance is independent. Not thread-safe: used by the thread of its owner only.
class TelemetryContext {
 public:
  TelemetryContext() noexcept;

  // Constructs a new telemetry context from configuration.
  explicit TelemetryContext(const TelemetryConfig& cfg);

  // Non-copyable, movable
  TelemetryContext(const TelemetryContext&) = delete;
  TelemetryContext& operator=(const TelemetryContext&) = delete;
  TelemetryContext(TelemetryContext&&) noexcept;
  TelemetryContext& operator=(TelemetryContext&&) noexcept;

  ~TelemetryContext();

  // Create a span with given name. Returns nullptr if tracing disabled or the span is not sampled.
  // Its spans are exported in batches of 512 (or every exportInterval, see flush()) by a thread of the context. While
  // that thread is busy, up to 2048 ended spans wait; the next ones are dropped (and counted in a warning).
  // Each span must be released (its SpanPtr destroyed or reset) before the context is destroyed or assigned.
  [[nodiscard]] SpanPtr createSpan(std::string_view name) const noexcept;

  // Whether tracing or metrics are enabled for this context.
  [[nodiscard]] bool enabled() const noexcept { return _impl != nullptr; }

  // Increment a counter by delta, with optional per-measurement labels. No-op if metrics disabled/failed.
  // The increments are summed in memory, per name and labels, until the next flush().
  void counterAdd(std::string_view name, uint64_t delta = 1UL, MetricLabels labels = {}) const noexcept;

  // Record a gauge value. No-op if metrics disabled/failed.
  void gauge(std::string_view name, int64_t value) const noexcept;

  // Record a gauge value with per-measurement labels. No-op if metrics disabled/failed.
  void gauge(std::string_view name, int64_t value, MetricLabels labels) const noexcept;

  // Record a histogram value. No-op if metrics disabled/failed.
  void histogram(std::string_view name, double value) const noexcept;

  // Record a histogram value with per-measurement labels. No-op if metrics disabled/failed.
  void histogram(std::string_view name, double value, MetricLabels labels) const noexcept;

  // Record a timing value. No-op if metrics disabled/failed.
  void timing(std::string_view name, std::chrono::milliseconds ms) const noexcept;

  // Record a timing value with per-measurement labels. No-op if metrics disabled/failed.
  void timing(std::string_view name, std::chrono::milliseconds ms, MetricLabels labels) const noexcept;

  // Exports the counters summed since the last flush, and sends the buffered DogStatsD metrics. Hands the ended spans
  // to the exporter thread when exportInterval has passed since the last batch.
  // SingleHttpServer flushes its context at each maintenance tick, and HttpClient after each request: flush a context
  // of your own periodically. The destructor flushes too.
  void flush() const noexcept;

  // Access underlying DogStatsD client, or nullptr if not enabled.
  // You can use it to emit custom DogStatsD metrics, buffered until the next flush().
  [[nodiscard]] DogStatsD* dogstatsdClient() const noexcept;

 private:
  std::unique_ptr<TelemetryContextImpl> _impl;
};

}  // namespace aeronet::tracing
