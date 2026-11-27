#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/string_view.h>
#include <opentelemetry/sdk/resource/resource.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/dogstatsd.hpp"
#include "aeronet/log.hpp"
#include "aeronet/metric-label.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "counter-accumulator.hpp"
#include "dogstatsd-metrics.hpp"
#include "otlp-span-export.hpp"
#include "span-recorder.hpp"
#include "span-sampler.hpp"

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
#include <opentelemetry/version.h>  // needed for OPENTELEMETRY_ABI_VERSION_NO NOLINT(misc-include-cleaner)

#include "aeronet/city-hash.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "otel-metric-labels.hpp"
#if OPENTELEMETRY_ABI_VERSION_NO < 2
#include <opentelemetry/context/context.h>

#include "gauge-deltas.hpp"
#endif
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

namespace {

inline auto OtelSv(std::string_view sv) { return opentelemetry::nostd::string_view(sv.data(), sv.size()); }

opentelemetry::sdk::resource::Resource buildTelemetryResource(const TelemetryConfig& cfg) {
  if (cfg.serviceName().empty()) {
    log::warn("Telemetry service name is empty; using default resource attributes");
    return opentelemetry::sdk::resource::Resource::Create({});
  }
  return opentelemetry::sdk::resource::Resource::Create({{"service.name", std::string(cfg.serviceName())}});
}

#ifdef AERONET_HAVE_METRICS_SDK
#if OPENTELEMETRY_ABI_VERSION_NO >= 2
using OtelGauge = opentelemetry::metrics::Gauge<int64_t>;
#else
// The ABI v1 - the default one of opentelemetry-cpp, built by vcpkg, Conan and the system packages - has no synchronous
// gauge: see GaugeDeltas.
using OtelGauge = opentelemetry::metrics::UpDownCounter<int64_t>;
#endif
#endif

// Seed of the span sampler and of the span ids: the contexts of a process draw different sequences.
uint64_t RandomSeed() {
  std::random_device randomDevice;
  return (static_cast<uint64_t>(randomDevice()) << 32U) ^ randomDevice();
}

}  // namespace

// TelemetryContext implementation details
class TelemetryContextImpl {
 public:
  explicit TelemetryContextImpl(const TelemetryConfig& cfg)
      : _spanSampler(cfg.sampleRate, RandomSeed()), _dogstatsd(cfg) {}

  TelemetryContextImpl(const TelemetryContextImpl&) = delete;
  TelemetryContextImpl(TelemetryContextImpl&&) = delete;
  TelemetryContextImpl& operator=(const TelemetryContextImpl&) = delete;
  TelemetryContextImpl& operator=(TelemetryContextImpl&&) = delete;

  ~TelemetryContextImpl() { flush(); }

  void flush() noexcept {
    _pendingCounters.flush([this](std::string_view name, uint64_t delta, MetricLabels labels) {
#ifdef AERONET_HAVE_METRICS_SDK
      if (_meter) {
        try {
          auto [it, inserted] = _counters.emplace(name, nullptr);
          if (inserted) {
            it->second = _meter->CreateUInt64Counter(OtelSv(name));
          }
          if (labels.empty()) {
            it->second->Add(delta);
          } else {
            it->second->Add(delta, detail::OtelMetricLabels(labels));
          }
        } catch (const std::exception& ex) {
          log::error("Failed to add counter '{}': {}", name, ex.what());
        }
      }
#endif
      _dogstatsd.increment(name, delta, labels);
    });
    _dogstatsd.flush();
    if (_spanRecorder) {
      _spanRecorder->flush();
    }
  }

#ifdef AERONET_HAVE_METRICS_SDK
  void recordGauge(std::string_view name, int64_t value, MetricLabels labels) noexcept {
    if (!_meter) {
      return;
    }
    try {
      auto [it, inserted] = _gauges.emplace(name, nullptr);
#if OPENTELEMETRY_ABI_VERSION_NO >= 2
      if (inserted) {
        it->second = _meter->CreateInt64Gauge(OtelSv(name));
      }
      if (labels.empty()) {
        it->second->Record(value);
      } else {
        it->second->Record(value, detail::OtelMetricLabels(labels));
      }
#else
      if (inserted) {
        it->second = _meter->CreateInt64UpDownCounter(OtelSv(name));
      }
      const int64_t delta = _gaugeDeltas.update(name, labels, value);
      if (labels.empty()) {
        it->second->Add(delta);
      } else {
        it->second->Add(delta, detail::OtelMetricLabels(labels));
      }
#endif
    } catch (const std::exception& ex) {
      log::error("Failed to set gauge '{}': {}", name, ex.what());
    }
  }

  void recordHistogram(std::string_view name, double value, MetricLabels labels) noexcept {
    if (!_meter) {
      return;
    }
    try {
      auto [it, inserted] = _histograms.emplace(name, nullptr);
      if (inserted) {
        it->second = _meter->CreateDoubleHistogram(OtelSv(name));
      }
#if OPENTELEMETRY_ABI_VERSION_NO >= 2
      if (labels.empty()) {
        it->second->Record(value);
      } else {
        it->second->Record(value, detail::OtelMetricLabels(labels));
      }
#else
      // The ABI v1 only records a histogram value with an explicit context.
      if (labels.empty()) {
        it->second->Record(value, opentelemetry::context::Context{});
      } else {
        it->second->Record(value, detail::OtelMetricLabels(labels), opentelemetry::context::Context{});
      }
#endif
    } catch (const std::exception& ex) {
      log::error("Failed to record histogram '{}': {}", name, ex.what());
    }
  }
#endif

  // Applies TelemetryConfig::sampleRate before a span is created.
  detail::SpanSampler _spanSampler;

#ifdef AERONET_HAVE_METRICS_SDK
  std::unique_ptr<opentelemetry::sdk::metrics::MeterProvider> _meterProvider;
  opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> _meter;
  // Keyed by views of the names stored in _pendingCounters.
  flat_hash_map<std::string_view, opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>, CityHash>
      _counters;
  flat_hash_map<std::string_view, opentelemetry::nostd::unique_ptr<OtelGauge>, CityHash> _gauges;
  flat_hash_map<std::string_view, opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>>, CityHash>
      _histograms;
#if OPENTELEMETRY_ABI_VERSION_NO < 2
  detail::GaugeDeltas _gaugeDeltas;
#endif
#endif

  detail::CounterAccumulator _pendingCounters;
  detail::DogStatsdMetrics _dogstatsd;
  // The spans, recorded and exported by aeronet, null when OpenTelemetry is disabled. Last: destroyed first, it exports
  // its last spans.
  std::unique_ptr<detail::SpanRecorder> _spanRecorder;
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

  const auto telemetryResource = buildTelemetryResource(cfg);

  // Spans end on the event loop thread: they must not be exported there. The recorder only appends an ended span to
  // its batch, exported by its own thread as soon as the batch is full, or at a flush() once exportInterval passed.
  ctx._spanRecorder = std::make_unique<detail::SpanRecorder>(detail::MakeOtlpHttpSpanSink(cfg),
                                                             detail::MakeOtlpSpanBatchHeader(telemetryResource),
                                                             cfg.exportInterval, RandomSeed());

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

  metricOpts.http_headers = detail::MakeOtlpHeaders(cfg);

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
  if (!_impl || !_impl->_spanRecorder || !_impl->_spanSampler.sample()) {
    return nullptr;
  }
  return _impl->_spanRecorder->createSpan(name);
}

void TelemetryContext::counterAdd(std::string_view name, uint64_t delta, MetricLabels labels) const noexcept {
  if (_impl) {
    _impl->_pendingCounters.add(name, delta, labels);
  }
}

void TelemetryContext::flush() const noexcept {
  if (_impl) {
    _impl->flush();
  }
}

void TelemetryContext::gauge(std::string_view name, int64_t value) const noexcept {
  if (_impl) {
#ifdef AERONET_HAVE_METRICS_SDK
    _impl->recordGauge(name, value, {});
#endif
    _impl->_dogstatsd.gauge(name, value);
  }
}

void TelemetryContext::gauge(std::string_view name, int64_t value, MetricLabels labels) const noexcept {
  if (_impl) {
#ifdef AERONET_HAVE_METRICS_SDK
    _impl->recordGauge(name, value, labels);
#endif
    _impl->_dogstatsd.gauge(name, value, labels);
  }
}

void TelemetryContext::histogram(std::string_view name, double value) const noexcept {
  if (_impl) {
#ifdef AERONET_HAVE_METRICS_SDK
    _impl->recordHistogram(name, value, {});
#endif
    _impl->_dogstatsd.histogram(name, value);
  }
}

void TelemetryContext::histogram(std::string_view name, double value, MetricLabels labels) const noexcept {
  if (_impl) {
#ifdef AERONET_HAVE_METRICS_SDK
    _impl->recordHistogram(name, value, labels);
#endif
    _impl->_dogstatsd.histogram(name, value, labels);
  }
}

void TelemetryContext::timing(std::string_view name, std::chrono::milliseconds ms) const noexcept {
  if (_impl) {
#ifdef AERONET_HAVE_METRICS_SDK
    _impl->recordGauge(name, ms.count(), {});
#endif
    _impl->_dogstatsd.timing(name, ms);
  }
}

void TelemetryContext::timing(std::string_view name, std::chrono::milliseconds ms, MetricLabels labels) const noexcept {
  if (_impl) {
#ifdef AERONET_HAVE_METRICS_SDK
    _impl->recordGauge(name, ms.count(), labels);
#endif
    _impl->_dogstatsd.timing(name, ms, labels);
  }
}

DogStatsD* TelemetryContext::dogstatsdClient() const noexcept {
  return _impl ? &_impl->_dogstatsd.dogstatsdClient() : nullptr;
}

}  // namespace aeronet::tracing
