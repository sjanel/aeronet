#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/sdk/trace/processor.h>
#include <opentelemetry/trace/tracer.h>

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/dogstatsd.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/log.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "dogstatsd-metrics.hpp"

// Detect SDK and processor support
#if __has_include( \
    <opentelemetry/sdk/trace/tracer_provider.h>) && __has_include(<opentelemetry/sdk/trace/simple_processor.h>)
#define AERONET_HAVE_OTEL_SDK 1
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/simple_processor.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/tracer_provider.h>

// Conditionally include TraceIdRatioBased sampler header (path differs across versions)
#if __has_include(<opentelemetry/sdk/trace/samplers/trace_id_ratio_based.h>)
#define AERONET_HAVE_TRACEID_RATIO 1
#include <opentelemetry/sdk/trace/samplers/trace_id_ratio_based.h>
#elif __has_include(<opentelemetry/sdk/trace/samplers/trace_id_ratio_based_sampler.h>)
#define AERONET_HAVE_TRACEID_RATIO 1
#include <opentelemetry/sdk/trace/samplers/trace_id_ratio_based_sampler.h>
#endif
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
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/metric_reader.h>
#include <opentelemetry/sdk/metrics/push_metric_exporter.h>
#include <opentelemetry/sdk/metrics/view/view_registry.h>
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

// Iterate over stored HTTP headers as (name, value) pairs; the callback receives string_view references.
void ForEachHttpHeader(const TelemetryConfig& cfg, const std::function<void(std::string_view, std::string_view)>& fn) {
  for (auto header : cfg.httpHeadersRange()) {
    const auto colonPos = header.find(':');
    if (colonPos == std::string_view::npos) {
      continue;
    }
    auto name = header.substr(0, colonPos);
    auto value = header.substr(colonPos + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    if (name.empty()) {
      continue;
    }
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

  void setAttribute(std::string_view key, int64_t val) noexcept override {
    if (!_span) {
      return;
    }
    try {
      _span->SetAttribute(opentelemetry::nostd::string_view(key.data(), key.size()), val);
    } catch (const std::exception& ex) {
      log::error("Failed to set span attribute '{}': {}", key, ex.what());
    } catch (...) {
      log::error("Failed to set span attribute '{}': unknown error", key);
    }
  }

  void setAttribute(std::string_view key, std::string_view val) noexcept override {
    if (!_span) {
      return;
    }
    try {
      _span->SetAttribute(opentelemetry::nostd::string_view(key.data(), key.size()),
                          opentelemetry::nostd::string_view(val.data(), val.size()));
    } catch (const std::exception& ex) {
      log::error("Failed to set span attribute '{}': {}", key, ex.what());
    } catch (...) {
      log::error("Failed to set span attribute '{}': unknown error", key);
    }
  }

  void end() noexcept override {
    if (!_span) {
      return;
    }
    try {
      _span->End();
    } catch (const std::exception& ex) {
      log::error("Failed to end span: {}", ex.what());
    } catch (...) {
      log::error("Failed to end span: unknown error");
    }
    _span = opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span>(nullptr);
  }

 private:
  opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> _span;
};

// TelemetryContext implementation details
class TelemetryContextImpl {
 public:
  explicit TelemetryContextImpl(const aeronet::TelemetryConfig& cfg) : _dogstatsd(cfg) {}

  opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider> _tracerProvider;
  opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> _tracer;

#ifdef AERONET_HAVE_METRICS_SDK
  std::shared_ptr<opentelemetry::sdk::metrics::MeterProvider> _meterProvider;
  opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> _meter;
  flat_hash_map<std::string_view, opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>>
      _counters;
#endif

  detail::DogStatsdMetrics _dogstatsd;
  bool _initialized = false;
};

TelemetryContext::TelemetryContext() noexcept = default;

TelemetryContext::TelemetryContext(const TelemetryConfig& cfg) : _impl(std::make_unique<TelemetryContextImpl>(cfg)) {
  if (!cfg.otelEnabled) {
    log::trace("Telemetry disabled in config");
    return;
  }

#if defined(AERONET_HAVE_OTLP_HTTP) || defined(AERONET_HAVE_OTLP_METRICS)
  const auto telemetryHeaders = buildOtlpHeaders(cfg);
#endif
  const auto telemetryResource = buildTelemetryResource(cfg);

  // Build trace exporter
#ifdef AERONET_HAVE_OTLP_HTTP
  opentelemetry::exporter::otlp::OtlpHttpExporterOptions opts;
  if (!cfg.endpoint().empty()) {
    opts.url = cfg.endpoint();
    log::info("Initializing OTLP HTTP trace exporter with endpoint: {}", cfg.endpoint());
  }
  opts.http_headers = telemetryHeaders;
  auto exporter = std::unique_ptr<opentelemetry::sdk::trace::SpanExporter>(
      new opentelemetry::exporter::otlp::OtlpHttpExporter(opts));
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
#ifdef AERONET_HAVE_TRACEID_RATIO
  auto sampler = std::unique_ptr<opentelemetry::sdk::trace::Sampler>(
      new opentelemetry::sdk::trace::TraceIdRatioBasedSampler(cfg.sampleRate));

  _impl->_tracerProvider = opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>(
      new opentelemetry::sdk::trace::TracerProvider(std::move(processor), telemetryResource, std::move(sampler)));
#else
  _impl->_tracerProvider = opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>(
      new opentelemetry::sdk::trace::TracerProvider(std::move(processor), telemetryResource));
#endif

  // Get tracer from this provider (NOT from global)
  _impl->_tracer = _impl->_tracerProvider->GetTracer("aeronet", AERONET_VERSION_STR);

  if (!_impl->_tracer) {
    throw std::runtime_error("Failed to get tracer from provider");
  }

  // Initialize metrics provider if SDK available
#if defined(AERONET_HAVE_METRICS_SDK) && defined(AERONET_HAVE_OTLP_METRICS)
  opentelemetry::exporter::otlp::OtlpHttpMetricExporterOptions metric_opts;
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
    metric_opts.url = std::move(endpoint);
  }

  metric_opts.http_headers = telemetryHeaders;

  auto metric_exporter = std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter>(
      new opentelemetry::exporter::otlp::OtlpHttpMetricExporter(metric_opts));

  opentelemetry::sdk::metrics::PeriodicExportingMetricReaderOptions reader_opts;
  reader_opts.export_interval_millis = std::chrono::milliseconds(5000);  // Export every 5s
  reader_opts.export_timeout_millis = std::chrono::milliseconds(3000);

  auto metric_reader = std::shared_ptr<opentelemetry::sdk::metrics::MetricReader>(
      new opentelemetry::sdk::metrics::PeriodicExportingMetricReader(std::move(metric_exporter), reader_opts));

  // Create MeterProvider - keep it in this instance, NO global singleton
  _impl->_meterProvider = std::make_shared<opentelemetry::sdk::metrics::MeterProvider>(
      std::make_unique<opentelemetry::sdk::metrics::ViewRegistry>(), telemetryResource);

  // Add the metric reader to the provider
  _impl->_meterProvider->AddMetricReader(metric_reader);

  // Get meter from this provider (NOT from global)
  _impl->_meter = _impl->_meterProvider->GetMeter("aeronet", AERONET_VERSION_STR);

  if (!_impl->_meter) {
    log::error("Failed to get meter from provider");
  } else {
    log::info("Metrics provider initialized successfully");
  }

#else
  log::warn("Metrics SDK not available - metrics disabled");
#endif

  _impl->_initialized = true;
}

TelemetryContext::~TelemetryContext() {
  // Shutdown providers
  if (_impl && _impl->_initialized) {
#ifdef AERONET_HAVE_METRICS_SDK
    if (_impl->_meterProvider) {
      _impl->_meterProvider->Shutdown();
    }
#endif
    // TracerProvider shutdown is handled by SDK
  }
}

TelemetryContext::TelemetryContext(TelemetryContext&&) noexcept = default;
TelemetryContext& TelemetryContext::operator=(TelemetryContext&&) noexcept = default;

SpanPtr TelemetryContext::createSpan(std::string_view name) const noexcept {
  if (!_impl || !_impl->_initialized || !_impl->_tracer) {
    return nullptr;
  }

  try {
    auto span = _impl->_tracer->StartSpan(opentelemetry::nostd::string_view(name.data(), name.size()));
    if (!span) {
      log::error("Failed to create span '{}'", name);
      return nullptr;
    }
    return std::make_unique<OtelSpan>(span);
  } catch (const std::exception& ex) {
    log::error("Failed to create span '{}': {}", name, ex.what());
    return nullptr;
  } catch (...) {
    log::error("Failed to create span '{}': unknown error", name);
    return nullptr;
  }
}

void TelemetryContext::counterAdd([[maybe_unused]] std::string_view name,
                                  [[maybe_unused]] uint64_t delta) const noexcept {
#ifdef AERONET_HAVE_METRICS_SDK
  if (_impl && _impl->_initialized && _impl->_meter) {
    try {
      auto [it, inserted] = _impl->_counters.emplace(name, nullptr);
      if (inserted) {
        it->second = _impl->_meter->CreateUInt64Counter(opentelemetry::nostd::string_view(name.data(), name.size()),
                                                        "Total count", "1");
      }
      it->second->Add(delta);
    } catch (const std::exception& ex) {
      log::error("Failed to add counter '{}': {}", name, ex.what());
    } catch (...) {
      log::error("Failed to add counter '{}': unknown error", name);
    }
  }
#endif
  if (_impl) {
    _impl->_dogstatsd.increment(name, delta);
  }
}

const DogStatsD* TelemetryContext::dogstatsdClient() const noexcept {
  if (_impl) {
    return &_impl->_dogstatsd.dogstatsdClient();
  }
  return nullptr;
}

#endif  // AERONET_HAVE_OTEL_SDK

}  // namespace aeronet::tracing