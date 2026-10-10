# Observability and logging

`aeronet` provides opt-in logging and OpenTelemetry integration so applications can expose protocol and request behavior without embedding a global logging policy in the library.

## Logging

aeronet logs through `<aeronet/log.hpp>`. With `AERONET_ENABLE_SPDLOG`, `aeronet::log` is spdlog, with its sinks and formatting. Without it, a small built-in logger with the same calls writes to standard output, with ISO 8601 UTC timestamps at millisecond precision. Levels are `trace`, `debug`, `info`, `warn`, `error`, and `critical`:

```cpp
#include <aeronet/log.hpp>

log::set_level(log::level::warn);  // runtime level, process-wide
log::info("server listening on {}", 8080);
```

The library logs failures with their context (connection, file, OpenSSL reason) at `error` or `warn`, lifecycle events at `info`, and per-request details at `debug` and below, which are compiled in but filtered by the level. Keep `info` or `warn` in production.

### Access log

The access log writes one line per request, independently of the logger:

```cpp
HttpServerConfig config;
config.accessLog.sink = AccessLogConfig::Sink::File;
config.accessLog.filePath = "/var/log/orders/access.log";
config.accessLog.format = AccessLogConfig::Format::JSON;
```

| `AccessLogConfig` | Default | Effect |
| --- | --- | --- |
| `sink` | `None` | `None` (disabled, no cost), `Stdout`, or `File` (appends to `filePath`). |
| `format` | `CLF` | Combined Log Format, or `JSON` (requires Glaze). |
| `useForwardedFor` | `false` | Log the first address of `X-Forwarded-For` instead of the peer address. |
| `flushThresholdInBytes` | 8 KiB | Lines are buffered and written once this size is reached, at maintenance ticks, and at shutdown. |

`useForwardedFor` is a trust-boundary setting, not a convenience: enable it only behind a proxy that replaces or sanitizes `X-Forwarded-For`, since clients can send any value. The access log can be changed at runtime with `postConfigUpdate()`; a file that cannot be opened rejects the update. Do not log request bodies, credentials, cookies, or tokens.

### Request metrics callback

For custom accounting, `setMetricsCallback()` receives a `RequestMetrics` record after each request, on the event-loop thread:

```cpp
SingleHttpServer server(HttpServerConfig{});
server.setMetricsCallback([](const RequestMetrics& metrics) {
  // metrics.method, path, status, bytesIn, bytesOut, duration, clientIp, userAgent, reusedConnection
});
```

The views of the record are valid during the call only. Keep the callback short: it runs for every request.

### Server statistics

`server.stats()` returns a snapshot of per-server counters, serializable with `ServerStats::json_str()`: output backpressure (see [Performance](performance.md#measuring)), TLS handshakes and kTLS usage (see [TLS observability](../protocols/tls.md#observability)), and event loop errors. `MultiHttpServer::stats()` returns the counters of each worker and their sum.

## OpenTelemetry and DogStatsD

OpenTelemetry support is optional. Configure with `AERONET_ENABLE_OPENTELEMETRY=ON` when the application needs OTLP traces or metrics, then enable it per server:

```cpp
#include <utility>

TelemetryConfig telemetry;
telemetry.otelEnabled = true;
telemetry.withEndpoint("http://otel-collector:4318")
    .withServiceName("orders")
    .withSampleRate(0.25)
    .addHttpHeader("authorization", "Bearer <collector-token>");

HttpServerConfig config;
config.withTelemetryConfig(std::move(telemetry));
```

The OTLP exporter sends over HTTP. Each `SingleHttpServer` owns an independent telemetry context; aeronet does not install a process global provider. `TelemetryConfig::endpoint()` is used as the trace endpoint, and aeronet derives `/v1/metrics` for the metric exporter. Export intervals, timeouts, trace sampling, exporter HTTP headers, and histogram buckets are all instance-specific.

Ended spans are only queued by the event loop: a background thread exports them in batches, at the latest `exportInterval` after they end, so a slow or unreachable collector never delays request processing. A span ending while the queue is full (2048 spans by default) is dropped.

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

When telemetry is disabled, each detailed HTTP/2 instrumentation point is a predictable null check and does not allocate. Instrument recording, span recording and DogStatsD datagram emission occur on the event-loop thread; OTLP metric and span exports run periodically on background threads. Keep DogStatsD sockets local and non-blocking and configure OTLP export intervals appropriately. Existing unlabeled metric overloads keep their original call shape; only calls that supply labels construct and visit a `MetricLabels` span.

The implementation adds no counters or timestamps to `Http2Stream` or to the protocol handler's per-stream request state. Per-stream duration reuses the request start timestamp that already exists. `TelemetryContext` remains one pointer and `Http2Stream` remains 32 bytes on the project's 64-bit build; `Http2Connection` gains one optional pointer (8 bytes) for the telemetry destination.

### Built-in instrumentation

Without handler code, each server emits:

| Signal | Name | Content |
| --- | --- | --- |
| Span | `http.request` | One per request, with `http.method`, `http.target`, `http.scheme`, `http.host`, `http.status_code`, and `http.duration_us`. |
| Span | `aeronet.middleware` | One per middleware call, with its phase, scope, index, and whether it short-circuited, threw, or ran for a streaming route. |
| Counters | `aeronet.connections.accepted`, `aeronet.events.processed`, `aeronet.events.errors`, `aeronet.bytes.read` | Connection and event loop activity. |
| Counters | `aeronet.connections.closed_for_*` | Connections closed by a timeout (`keep_alive`, `header_read_timeout`, `body_read_timeout`, `request_timeout`, `handshake_timeout`) or a drain (`drain`). |
| Counters | `aeronet.http_responses.compression.*` | Response compression attempts, results above `maxCompressRatio`, and errors. |
| HTTP/2 instruments | `aeronet.http2.*` | See [Built-in HTTP/2 metrics](#built-in-http2-metrics). |

The HTTP client emits `aeronet.http_requests.*` counters through its own `TelemetryConfig` (retries, redirects, request compression).

### Collector setup

The OTLP exporter speaks OTLP over HTTP (port 4318 by convention). A minimal OpenTelemetry Collector configuration that prints what it receives, for testing:

```yaml
receivers:
  otlp:
    protocols:
      http:
        endpoint: 0.0.0.0:4318
exporters:
  debug:
    verbosity: detailed
service:
  pipelines:
    traces:
      receivers: [otlp]
      exporters: [debug]
    metrics:
      receivers: [otlp]
      exporters: [debug]
```

Building with `AERONET_ENABLE_OPENTELEMETRY` requires curl and protobuf development packages:

| Distribution | Packages |
| --- | --- |
| Debian, Ubuntu | `libcurl4-openssl-dev libprotobuf-dev protobuf-compiler` |
| Fedora, RHEL | `libcurl-devel protobuf-devel protobuf-compiler` |
| Alpine | `curl-dev protobuf-dev protobuf-c-compiler` |
| Arch | `curl protobuf` |

## Health probes and deployment

The server includes Kubernetes-style probe support. Keep a probe endpoint lightweight, make readiness reflect dependencies that actually gate traffic, and consider a dedicated listener when probe availability must be isolated from application load.

Read [Health probes](health-probes.md), then use the [Kubernetes deployment guide](../kubernetes-examples.md) for ConfigMap and manifest examples.

## Tests

- OpenTelemetry export end to end: [tests/opentelemetry-e2e_test.cpp](../../tests/opentelemetry-e2e_test.cpp).
- DogStatsD: [dogstatsd_test.cpp](../../aeronet/objects/test/dogstatsd_test.cpp).
- Access log: [access-log-writer_test.cpp](../../aeronet/server/test/access-log-writer_test.cpp).
