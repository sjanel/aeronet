#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/sdk/trace/processor.h>
#include <opentelemetry/trace/tracer.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/dogstatsd.hpp"
#include "aeronet/log.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "dogstatsd-metrics.hpp"

// Detect SDK and processor support
#if __has_include( \
    <opentelemetry/sdk/trace/tracer_provider.h>) && __has_include(<opentelemetry/sdk/trace/simple_processor.h>)
#define AERONET_HAVE_OTEL_SDK 1
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/sampler.h>
#include <opentelemetry/sdk/trace/samplers/trace_id_ratio.h>
#include <opentelemetry/sdk/trace/simple_processor.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/tracer_provider.h>

#endif

// Prefer OTLP HTTP exporter (requires curl client). Fallback to ostream exporter.
#if __has_include( \
    <opentelemetry/exporters/otlp/otlp_http_exporter.h>) && __has_include(<opentelemetry/ext/http/client/http_client.h>)
#define AERONET_HAVE_OTLP_HTTP 1
#include <opentelemetry/exporters/otlp/otlp_environment.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/trace/exporter.h>
#elif __has_include(<opentelemetry/exporters/ostream/span_exporter.h>)
#define AERONET_HAVE_OSTREAM_EXPORTER 1
#include <opentelemetry/exporters/ostream/span_exporter.h>
#endif

// Detect metrics SDK support for MeterProvider
#if __has_include(<opentelemetry/sdk/metrics/meter_provider.h>)
#define AERONET_HAVE_METRICS_SDK 1
#include <opentelemetry/metrics/sync_instruments.h>
#include <opentelemetry/nostd/unique_ptr.h>
#include <opentelemetry/sdk/metrics/aggregation/aggregation_config.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/instruments.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/metric_reader.h>
#include <opentelemetry/sdk/metrics/push_metric_exporter.h>
#include <opentelemetry/sdk/metrics/view/instrument_selector_factory.h>
#include <opentelemetry/sdk/metrics/view/meter_selector_factory.h>
#include <opentelemetry/sdk/metrics/view/view.h>
#include <opentelemetry/sdk/metrics/view/view_registry.h>

#include "aeronet/city-hash.hpp"
#include "aeronet/flat-hash-map.hpp"
#endif

// Detect OTLP metrics exporter
#if __has_include(<opentelemetry/exporters/otlp/otlp_http_metric_exporter.h>)
#define AERONET_HAVE_OTLP_METRICS 1
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h>
#endif

namespace aeronet::tracing {

#ifdef AERONET_HAVE_OTEL_SDK

namespace {

namespace {
constexpr auto OtelSv(std::string_view sv) { return opentelemetry::nostd::string_view(sv.data(), sv.size()); }
}  // namespace

// Iterate over stored HTTP headers as (name, value) pairs; the callback receives string_view references.
void ForEachHttpHeader(const TelemetryConfig& cfg, const std::function<void(std::string_view, std::string_view)>& fn) {
  for (auto header : cfg.httpHeadersRange()) {
    const auto colonPos = header.find(':');
    const auto name = header.substr(0, colonPos);
    const auto value = header.substr(colonPos + 1);

    fn(name, value);
  }
}

#if defined(AERONET_HAVE_OTLP_HTTP) || defined(AERONET_HAVE_OTLP_METRICS)
opentelemetry::exporter::otlp::OtlpHeaders buildOtlpHeaders(const TelemetryConfig& cfg) {
  opentelemetry::exporter::otlp::OtlpHeaders headers;
  ForEachHttpHeader(cfg, [&](std::string_view name, std::string_view value) {
    headers.emplace(std::string(name), std::string(value));
  });
  return headers;
}
#endif

opentelemetry::sdk::resource::Resource buildTelemetryResource(const TelemetryConfig& cfg) {
  if (cfg.serviceName().empty()) {
    log::warn("Telemetry service name is empty; using default resource attributes");
    return opentelemetry::sdk::resource::Resource::Create({});
  }
  return opentelemetry::sdk::resource::Resource::Create({{"service.name", std::string(cfg.serviceName())}});
}

}  // namespace

// OpenTelemetry Span implementation
class OtelSpan final : public Span {
 public:
  explicit OtelSpan(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span) noexcept
      : _span(std::move(span)) {}

  void setAttribute(std::string_view key, int64_t val) noexcept override { _span->SetAttribute(OtelSv(key), val); }

  void setAttribute(std::string_view key, std::string_view val) noexcept override {
    _span->SetAttribute(OtelSv(key), OtelSv(val));
  }

  void end() noexcept override { _span->End(); }

 private:
  opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> _span;
};

// TelemetryContext implementation details
class TelemetryContextImpl {
 public:
  explicit TelemetryContextImpl(const TelemetryConfig& cfg) : _dogstatsd(cfg) {}

  opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider> _tracerProvider;
  opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> _tracer;

#ifdef AERONET_HAVE_METRICS_SDK
  std::unique_ptr<opentelemetry::sdk::metrics::MeterProvider> _meterProvider;
  opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> _meter;
  flat_hash_map<std::string_view, opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>, CityHash>
      _counters;
  flat_hash_map<std::string_view, opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Gauge<int64_t>>, CityHash>
      _gauges;
  flat_hash_map<std::string_view, opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>>, CityHash>
      _histograms;
#endif

  detail::DogStatsdMetrics _dogstatsd;
};

TelemetryContext::TelemetryContext() noexcept = default;

TelemetryContext::TelemetryContext(const TelemetryConfig& cfg) {
  auto impl = std::make_unique<TelemetryContextImpl>(cfg);
  TelemetryContextImpl& ctx = *impl;
  if (!cfg.otelEnabled) {
    log::trace("Telemetry disabled in config");
    if (cfg.dogStatsDEnabled) {
      _impl = std::move(impl);
    }
    return;
  }

#if defined(AERONET_HAVE_OTLP_HTTP) || defined(AERONET_HAVE_OTLP_METRICS)
  const auto telemetryHeaders = buildOtlpHeaders(cfg);
#endif
  const auto telemetryResource = buildTelemetryResource(cfg);

  // Build trace exporter
#ifdef AERONET_HAVE_OTLP_HTTP
  opentelemetry::exporter::otlp::OtlpHttpExporterOptions opts;
  if (cfg.endpoint().empty()) {
    log::warn("OTLP endpoint is empty; using default endpoint from environment or SDK defaults");
  } else {
    opts.url = cfg.endpoint();
    log::info("Initializing OTLP HTTP trace exporter with endpoint: {}", cfg.endpoint());
  }
  opts.http_headers = telemetryHeaders;
  auto exporter = std::unique_ptr<opentelemetry::sdk::trace::SpanExporter>(
      new opentelemetry::exporter::otlp::OtlpHttpExporter(std::move(opts)));  // NOLINT(performance-move-const-arg)
#elifdef AERONET_HAVE_OSTREAM_EXPORTER
  log::info("Initializing ostream trace exporter (OTLP not available)");
  auto exporter = std::unique_ptr<opentelemetry::sdk::trace::SpanExporter>(
      new opentelemetry::exporter::trace::OStreamSpanExporter());
#else
#error "No trace exporter available - neither OTLP HTTP nor ostream exporter found"
#endif

  // Processor (SimpleSpanProcessor for compatibility)
  auto processor = std::unique_ptr<opentelemetry::sdk::trace::SpanProcessor>(
      new opentelemetry::sdk::trace::SimpleSpanProcessor(std::move(exporter)));

  // Create provider - NO global singleton, keep it in this instance
  auto sampler = std::unique_ptr<opentelemetry::sdk::trace::Sampler>(
      new opentelemetry::sdk::trace::TraceIdRatioBasedSampler(cfg.sampleRate));

  ctx._tracerProvider = opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>(
      new opentelemetry::sdk::trace::TracerProvider(std::move(processor), telemetryResource, std::move(sampler)));

  // Get tracer from this provider (NOT from global)
  ctx._tracer = ctx._tracerProvider->GetTracer("aeronet", AERONET_VERSION_STR);
  assert(ctx._tracer != nullptr);

  // Initialize metrics provider if SDK available
#if defined(AERONET_HAVE_METRICS_SDK) && defined(AERONET_HAVE_OTLP_METRICS)
  opentelemetry::exporter::otlp::OtlpHttpMetricExporterOptions metricOpts;
  if (!cfg.endpoint().empty()) {
    // Convert trace endpoint to metrics endpoint
    // OTLP trace: /v1/traces, metrics: /v1/metrics
    std::string endpoint(cfg.endpoint());
    auto tracesPos = endpoint.find("/v1/traces");
    if (tracesPos != std::string::npos) {
      endpoint.replace(tracesPos, 10, "/v1/metrics");
    } else if (endpoint.back() == '/') {
      endpoint += "v1/metrics";
    } else {
      endpoint += "/v1/metrics";
    }
    log::info("Initializing OTLP HTTP metrics exporter with endpoint: {}", endpoint);
    metricOpts.url = std::move(endpoint);
  }

  metricOpts.http_headers = telemetryHeaders;

  auto metricExporter = std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter>(
      new opentelemetry::exporter::otlp::OtlpHttpMetricExporter(
          std::move(metricOpts)));  // NOLINT(performance-move-const-arg)

  opentelemetry::sdk::metrics::PeriodicExportingMetricReaderOptions readerOpts;
  readerOpts.export_interval_millis = cfg.exportInterval;
  readerOpts.export_timeout_millis = cfg.exportTimeout;

  auto metricReader = std::shared_ptr<opentelemetry::sdk::metrics::MetricReader>(
      new opentelemetry::sdk::metrics::PeriodicExportingMetricReader(std::move(metricExporter), std::move(readerOpts)));

  auto viewRegistry = std::make_unique<opentelemetry::sdk::metrics::ViewRegistry>();
  for (const auto& [metricName, boundaries] : cfg.histogramBuckets()) {
    auto histogramCfg = std::make_shared<opentelemetry::sdk::metrics::HistogramAggregationConfig>();
    histogramCfg->boundaries_.assign(boundaries.begin(), boundaries.end());

    std::string metricNameStr(metricName);

    auto instrumentSelector = opentelemetry::sdk::metrics::InstrumentSelectorFactory::Create(
        opentelemetry::sdk::metrics::InstrumentType::kHistogram, metricNameStr, "");
    auto meterSelector = opentelemetry::sdk::metrics::MeterSelectorFactory::Create("aeronet", AERONET_VERSION_STR, "");

    auto view = std::make_unique<opentelemetry::sdk::metrics::View>(
        metricNameStr, /*description=*/"", opentelemetry::sdk::metrics::AggregationType::kHistogram, histogramCfg);

    viewRegistry->AddView(std::move(instrumentSelector), std::move(meterSelector), std::move(view));
  }

  // Create MeterProvider - keep it in this instance, NO global singleton
  ctx._meterProvider =
      std::make_unique<opentelemetry::sdk::metrics::MeterProvider>(std::move(viewRegistry), telemetryResource);

  // Add the metric reader to the provider
  ctx._meterProvider->AddMetricReader(metricReader);
  // Get meter from this provider (NOT from global)
  ctx._meter = ctx._meterProvider->GetMeter("aeronet", AERONET_VERSION_STR);
  assert(ctx._meter != nullptr);

  log::debug("Metrics provider initialized successfully");

#else
  log::warn("Metrics SDK not available - metrics disabled");
#endif

  _impl = std::move(impl);
}

TelemetryContext::~TelemetryContext() = default;

TelemetryContext::TelemetryContext(TelemetryContext&&) noexcept = default;
TelemetryContext& TelemetryContext::operator=(TelemetryContext&&) noexcept = default;

SpanPtr TelemetryContext::createSpan(std::string_view name) const noexcept {
  if (!_impl || !_impl->_tracer) {
    return nullptr;
  }

  try {
    return std::make_unique<OtelSpan>(_impl->_tracer->StartSpan(OtelSv(name)));
  } catch (const std::exception& ex) {
    log::error("Failed to create span '{}': {}", name, ex.what());
    return nullptr;
  }
}

void TelemetryContext::counterAdd(std::string_view name, uint64_t delta) const noexcept {
  if (_impl) {
#ifdef AERONET_HAVE_METRICS_SDK
    if (_impl->_meter) {
      try {
        auto [it, inserted] = _impl->_counters.emplace(name, nullptr);
        if (inserted) {
          it->second = _impl->_meter->CreateUInt64Counter(OtelSv(name));
        }
        it->second->Add(delta);
      } catch (const std::exception& ex) {
        log::error("Failed to add counter '{}': {}", name, ex.what());
      }
    }
#endif
    _impl->_dogstatsd.increment(name, delta);
  }
}

void TelemetryContext::gauge(std::string_view name, int64_t value) const noexcept {
  if (_impl) {
#ifdef AERONET_HAVE_METRICS_SDK
    if (_impl->_meter) {
      try {
        auto [it, inserted] = _impl->_gauges.emplace(name, nullptr);
        if (inserted) {
          it->second = _impl->_meter->CreateInt64Gauge(OtelSv(name));
        }
        it->second->Record(value);
      } catch (const std::exception& ex) {
        log::error("Failed to set gauge '{}': {}", name, ex.what());
      }
    }
#endif
    _impl->_dogstatsd.gauge(name, value);
  }
}

void TelemetryContext::histogram(std::string_view name, double value) const noexcept {
  if (_impl) {
#ifdef AERONET_HAVE_METRICS_SDK
    if (_impl->_meter) {
      try {
        auto [it, inserted] = _impl->_histograms.emplace(name, nullptr);
        if (inserted) {
          it->second = _impl->_meter->CreateDoubleHistogram(OtelSv(name));
        }
        it->second->Record(value);
      } catch (const std::exception& ex) {
        log::error("Failed to record histogram '{}': {}", name, ex.what());
      }
    }
#endif
    _impl->_dogstatsd.histogram(name, value);
  }
}

void TelemetryContext::timing(std::string_view name, std::chrono::milliseconds ms) const noexcept {
  if (_impl) {
#ifdef AERONET_HAVE_METRICS_SDK
    if (_impl->_meter) {
      try {
        auto [it, inserted] = _impl->_gauges.emplace(name, nullptr);
        if (inserted) {
          it->second = _impl->_meter->CreateInt64Gauge(OtelSv(name));
        }
        it->second->Record(std::chrono::duration_cast<std::chrono::milliseconds>(ms).count());
      } catch (const std::exception& ex) {
        log::error("Failed to set gauge '{}': {}", name, ex.what());
      }
    }
#endif
    _impl->_dogstatsd.timing(name, ms);
  }
}

DogStatsD* TelemetryContext::dogstatsdClient() const noexcept {
  return _impl ? &_impl->_dogstatsd.dogstatsdClient() : nullptr;
}

#endif  // AERONET_HAVE_OTEL_SDK

}  // namespace aeronet::tracing