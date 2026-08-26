# Observability and logging

`aeronet` provides opt-in logging and OpenTelemetry integration so applications can expose protocol and request behavior without embedding a global logging policy in the library.

## Logging

The logging module integrates with spdlog when enabled. Configure the sink, level, and format for the environment that runs your service. Include enough request context to diagnose failures without placing sensitive payloads or credentials in logs.

See [Logging](../FEATURES.md#logging) for the supported configuration and structured access-log behavior.

```cpp
using namespace std::chrono_literals;

HttpServerConfig config;
config.accessLog.sink = AccessLogConfig::Sink::Stdout;
config.accessLog.format = AccessLogConfig::Format::JSON;
config.accessLog.useForwardedFor = true;  // Only when the trusted proxy sanitizes it.

TelemetryConfig telemetry;
telemetry.otelEnabled = true;
telemetry.withEndpoint("http://otel-collector:4318")
    .withServiceName("orders")
    .withSampleRate(0.25)
    .addHttpHeader("authorization", "Bearer <collector-token>");
config.withTelemetryConfig(std::move(telemetry));
```

`useForwardedFor` is a trust-boundary setting, not a general convenience switch. Enable it only when a trusted proxy replaces or sanitizes `X-Forwarded-For`. Do not log request bodies, authorization credentials, cookies, or collector tokens.

## OpenTelemetry and DogStatsD

OpenTelemetry support is optional. Configure with `AERONET_ENABLE_OPENTELEMETRY=ON` when the application needs OTLP traces or metrics. Each `SingleHttpServer` owns an independent telemetry context; aeronet does not install a process global provider. `TelemetryConfig::endpoint()` is used as the trace endpoint, and aeronet derives `/v1/metrics` for the metric exporter. Export intervals, timeouts, trace sampling, exporter HTTP headers, and histogram buckets are all instance-specific.

DogStatsD metrics do not require OpenTelemetry at build time. They can be enabled alone or together with OTLP:

```cpp
TelemetryConfig telemetry;
telemetry.enableDogStatsDMetrics()
    .withDogStatsdSocketPath("/var/run/datadog/dsd.socket")
    .withDogStatsdNamespace("orders")
    .addDogStatsdTag("env:production")
    .addDogStatsdTag("region:eu-west-1");

HttpServerConfig config;
config.withTelemetryConfig(std::move(telemetry));
```

When both exporters are enabled, every metric is sent to both. `addDogStatsdTag()` configures DogStatsD-only tags that are included on every measurement. If `dogstatsdNamespace` is empty, `serviceName` is used as its namespace. Built-in metric names already start with `aeronet.`, so do not set the namespace itself to `aeronet` unless the resulting `aeronet.aeronet.*` prefix is intentional.

### Metric labels

All metric operations accept an optional `MetricLabels` span. Both OTLP and DogStatsD receive the same per-measurement labels:

```cpp
#include <aeronet/tracing/tracer.hpp>

using namespace aeronet;

TelemetryConfig config;
config.otelEnabled = true;
config.withEndpoint("http://otel-collector:4318").withServiceName("orders");

tracing::TelemetryContext telemetry(config);
const MetricLabel labels[]{
    {"operation", "checkout"},
    {"result", "accepted"},
};

telemetry.counterAdd("orders.requests", 1, labels);
telemetry.gauge("orders.queue.depth", 12, labels);
telemetry.histogram("orders.payload.bytes", 1536.0, labels);
telemetry.timing("orders.latency", std::chrono::milliseconds{8}, labels);
```

`MetricLabel` stores two `std::string_view` values. The array and referenced strings only need to remain valid until the metric call returns. The API does not allocate or copy a label container. DogStatsD appends the labels after configured global tags in `key:value` form; OpenTelemetry records them as data-point attributes.
Label keys and values must use the character set accepted by the configured exporter; aeronet deliberately does not escape or validate them on the metric hot path.

Use labels for bounded dimensions such as operation, status class, protocol direction, or frame type. Avoid user IDs, request IDs, URLs, connection IDs, and HTTP/2 stream IDs because every distinct label set creates another metric time series. Histogram bucket configuration is keyed by instrument name, not by its labels:

```cpp
TelemetryConfig telemetryConfig;
constexpr double ratioBuckets[]{0.1, 0.25, 0.5, 0.75, 1.0, 2.0};
telemetryConfig.addHistogramBuckets("aeronet.http2.hpack.compression.ratio", ratioBuckets);
```

DogStatsD performs histogram aggregation in the agent or backend, so these client-side bucket boundaries apply only to OpenTelemetry.

### Built-in HTTP/2 metrics

Detailed HTTP/2 metrics are emitted automatically for server connections whenever telemetry metrics are enabled.

| Instrument | Kind | Unit | Labels | Meaning |
| --- | --- | --- | --- | --- |
| `aeronet.http2.frames` | Counter | frames | `direction`, `frame.type` | Complete inbound frames accepted for processing and outbound frames queued by type. |
| `aeronet.http2.frame.payload.bytes` | Counter | bytes | `direction`, `frame.type` | Frame payload bytes, excluding the 9-byte HTTP/2 frame header. Zero-length payloads add to the frame counter only. |
| `aeronet.http2.streams.opened` | Counter | streams | `initiator` | Streams admitted by the connection. `initiator` is `local` or `remote`. |
| `aeronet.http2.streams.closed` | Counter | streams | `initiator`, `error.type` | Streams closed normally or by reset. `error.type` uses HTTP/2 names such as `NO_ERROR` and `CANCEL`. |
| `aeronet.http2.streams.active` | Histogram | streams | none | Active stream count on a connection after every open or close transition. This is a per-connection distribution, not a server-wide gauge. |
| `aeronet.http2.stream.requests` | Counter | requests | `http.request.method`, `http.response.status_code` | Completed server request streams. |
| `aeronet.http2.stream.duration` | Histogram | milliseconds | `http.request.method`, `http.response.status_code` | Time from the decoded request head to response completion. |
| `aeronet.http2.stream.request.body.bytes` | Histogram | bytes | `http.request.method`, `http.response.status_code` | Decoded request body size for each completed stream. |
| `aeronet.http2.hpack.block.compressed.bytes` | Histogram | bytes | `direction` | Encoded HPACK block size. |
| `aeronet.http2.hpack.block.header_list.bytes` | Histogram | bytes | `direction` | Decoded HPACK header-list size: name bytes + value bytes + the RFC 7541 32-byte overhead for every field. |
| `aeronet.http2.hpack.compression.ratio` | Histogram | ratio | `direction` | `compressed bytes / header-list bytes`; lower values mean better compression. Empty header lists do not record a ratio. |

`direction` is `received` or `sent`. `frame.type` is one of `data`, `headers`, `priority`, `rst_stream`, `settings`, `push_promise`, `ping`, `goaway`, `window_update`, `continuation`, or `unknown` for an extension frame. The frame counters measure protocol work, not confirmed network delivery: a queued outbound frame is counted even if the transport closes before it is written.

Each stream completion contributes one sample to the duration and body-size histograms. There is intentionally no `stream.id` label. A stream identifier is connection-local and unbounded, so exporting it would create high-cardinality series without uniquely identifying a stream across connections. Use request traces or an application correlation ID in logs when an individual request must be followed.

Useful starting points for dashboards and alerts include:

- rate of `frames` grouped by `direction` and `frame.type`;
- `rst_stream` share and `streams.closed` grouped by `error.type`;
- active-stream distribution and high-water percentiles against the configured concurrent-stream limit;
- p50/p95/p99 stream duration grouped by method and status;
- HPACK ratio and compressed/header-list bytes grouped by direction.

### Runtime cost and object layout

When telemetry is disabled, each detailed HTTP/2 instrumentation point is a predictable null check and does not allocate. Instrument recording and DogStatsD datagram emission occur on the event-loop thread; OTLP metric export runs periodically. Keep DogStatsD sockets local and non-blocking and configure OTLP export intervals appropriately. Existing unlabeled metric overloads keep their original call shape; only calls that supply labels construct and visit a `MetricLabels` span.

The implementation adds no counters or timestamps to `Http2Stream` or to the protocol handler's per-stream request state. Per-stream duration reuses the request start timestamp that already exists. `TelemetryContext` remains one pointer and `Http2Stream` remains 32 bytes on the project's 64-bit build; `Http2Connection` gains one optional pointer (8 bytes) for the telemetry destination.

The [OpenTelemetry reference](../FEATURES.md#opentelemetry-integration) describes the broader instrumentation surface, collector configuration, and dependency requirements.

## Health probes and deployment

The server includes Kubernetes-style probe support. Keep a probe endpoint lightweight, make readiness reflect dependencies that actually gate traffic, and consider a dedicated listener when probe availability must be isolated from application load.

Read [Built-in Kubernetes-style probes](../FEATURES.md#built-in-kubernetes-style-probes), then use the [Kubernetes deployment guide](../kubernetes-examples.md) for ConfigMap and manifest examples.
