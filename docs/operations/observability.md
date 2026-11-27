# Observability and logging

`aeronet` provides opt-in logging and OpenTelemetry integration so applications can expose protocol and request behavior without embedding a global logging policy in the library.

## Logging

aeronet logs through `<aeronet/log.hpp>`, with the calls of spdlog: `log::trace()`, `debug()`, `info()`, `warn()`, `error()` and `critical()` take a format string, checked at compile time, and its arguments (without arguments, the text is written as is), `log::set_level()` and `log::get_level()` the level (`log::level::trace` to `critical`, and `off`):

```cpp
#include <aeronet/log.hpp>

log::set_level(log::level::warn);  // runtime level, process-wide
log::info("server listening on {}", 8080);
```

The format strings are those of `std::format`, with the same output, for arguments of type `bool`, `char`, the integer types, `float`, `double`, strings (`std::string`, `std::string_view`, C strings) and `void` pointers: `{}` and `{:spec}` with the standard format specification of the type, except the locale-specific form `L`, the debug form `?`, dynamic width or precision (`{:{}}`) and argument indexes (`{0}`). Each argument must have its replacement field. `<aeronet/log.hpp>` does not include `<format>`, whose parsing alone costs about 0.2 s per source file: format a value of another type into a string first (`std::format` with a `std::formatter` of your own).

A call below the current level costs a load and a comparison (0.2 ns, against 2 ns through spdlog directly), and the calls never throw: a message that cannot be formatted or written (`{:c}` of an integer that is not a character, a failed allocation) is dropped, with a line on standard error.

With `AERONET_ENABLE_SPDLOG`, the messages are written by the default logger of spdlog, with its sinks and pattern. `log::set_level()` also sets the level of the spdlog loggers, but a level set through spdlog only does not change which messages aeronet writes. spdlog is compiled into `aeronet_tech` only: `<aeronet/log.hpp>` does not include it. Without it, a small built-in logger writes to standard output, with ISO 8601 UTC timestamps at millisecond precision, each line in a single write: the lines logged by concurrent threads do not interleave.

The library logs failures with their context (connection, file, OpenSSL reason) at `error` or `warn`, lifecycle events at `info`, and per-request details at `debug` and below, which are compiled in but filtered by the level. Keep `info` or `warn` in production. Tests: [log_test.cpp](../../aeronet/tech/test/log_test.cpp) and [format-lite_test.cpp](../../aeronet/tech/test/format-lite_test.cpp) (each accepted format specification against `std::format`), benchmarks: [log_bench.cpp](../../benchmarks/internal/log_bench.cpp) and [format-lite_bench.cpp](../../benchmarks/internal/format-lite_bench.cpp).

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

`reusedConnection` tells whether the connection served another request before this one. The views of the record are valid during the call only. Keep the callback short: it runs for every request.

### Server statistics

`server.stats()` returns a snapshot of per-server counters, serializable with `ServerStats::json_str()`: output backpressure and `MSG_ZEROCOPY` (see [Performance](performance.md#measuring)), TLS handshakes and kTLS usage (see [TLS observability](../protocols/tls.md#observability)), and event loop errors. `MultiHttpServer::stats()` returns the counters of each worker and their sum.

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

`withSampleRate(rate)` records that proportion of the spans, decided at random for each span before it is created: an unsampled request costs nothing, as with tracing disabled. aeronet does not read an incoming trace context (`traceparent`): each span starts a trace of its own, so that this draw samples like the trace id ratio sampler of the SDK.

### Spans

aeronet records its spans itself, not through the trace SDK of opentelemetry-cpp. It still uses opentelemetry-cpp for the resource (`service.name`, the `telemetry.sdk.*` attributes and `OTEL_RESOURCE_ATTRIBUTES`), the OTLP HTTP client, and the options that the `OTEL_EXPORTER_OTLP_TRACES_*` variables give it: the endpoint when `endpoint()` is empty, the timeout, TLS, compression and retries. Each span is encoded in OTLP protobuf as it is made, in a record reused from span to span: once warm, a sampled `http.request` span makes no allocation on the event loop and costs about 90 ns (1.65 µs and 35 allocations through the SDK; Release, see the [ROADMAP](../ROADMAP.md#tracing)). The exported spans are the ones the SDK would export: the same resource and instrumentation scope (`aeronet` and its version), random trace and span ids, kind internal, the sampled flag, start and end times, and the attributes of the span. An attribute value that is not valid UTF-8 is sent as bytes (`bytes_value`): in a string, it would make the collector reject the whole batch.

The event loop only appends an ended span to the current batch. A thread of the telemetry context exports each batch, handed off to it as soon as the batch has 512 spans, or at a flush (each maintenance tick of the server) once `exportInterval` has passed since the previous batch: one lock and one wakeup per batch, and a slow or unreachable collector never delays request processing. While that thread is busy with the previous batches, up to 2048 ended spans wait; the next ones are dropped and counted, in one warning per batch. The destruction of the server ends the spans of the requests still in progress (its connections are destroyed before its telemetry), exports them with the others, and waits for their export (at most the exporter timeout per batch, `OTEL_EXPORTER_OTLP_TRACES_TIMEOUT`, 10 s by default).

Unlike the batch span processor of the SDK, the `OTEL_BSP_*` variables are not read (512 and 2048 are their defaults), traces are always sent in protobuf (an `http/json` `OTEL_EXPORTER_OTLP_TRACES_PROTOCOL` is ignored with a warning), and the `OTEL_SPAN_*` limits do not apply: aeronet sets at most 8 attributes per span.

`TelemetryContext::createSpan()` returns a `tracing::SpanPtr`, a `std::unique_ptr<Span, SpanRecycler>` whose deleter calls `Span::recycle()` instead of `delete`: a `Span` implementation of your own, such as a test double, implements `recycle()` with `delete this`. The memory of a span belongs to its context: release each span (destroy or reset its `SpanPtr`) before its `TelemetryContext` is destroyed or assigned.

Tests: [span-recorder_test.cpp](../../aeronet/objects/test/span-recorder_test.cpp) (each batch compared with the request that the OTLP exporter of the SDK makes of the same spans, invalid UTF-8, the batch boundary, the flush, drops under a stalled collector, the last batch exported at destruction, more live spans than the first block of records), `TracesAreSentInProtobufWhenJsonIsAsked` and `TraceExportPartialSuccess` in [opentelemetry-e2e_test.cpp](../../tests/opentelemetry-e2e_test.cpp), and the benchmark [tracing-span_bench.cpp](../../benchmarks/internal/tracing-span_bench.cpp).

### opentelemetry-cpp ABI

aeronet builds with both ABIs of opentelemetry-cpp: the ABI v1, its default, built by the vcpkg and Conan packages and by the system ones, and the ABI v2, the one of the opentelemetry-cpp that aeronet downloads when none is installed. The ABI only matters for metrics: spans do not go through the trace API of opentelemetry-cpp. The ABI v1 has no synchronous gauge: `gauge()` and `timing()` are then exported as up-down counters (non-monotonic sums) whose value is the last one recorded, per label set, instead of gauges. Prometheus maps both to a gauge. With the ABI v1, give the labels of a gauge in the same order at each call: the last value is remembered per label sequence. Tests: [gauge-deltas_test.cpp](../../aeronet/objects/test/gauge-deltas_test.cpp), [opentelemetry-e2e_test.cpp](../../tests/opentelemetry-e2e_test.cpp) (`GaugesExportTheirLastValue`).

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

### Batching and flush

Some metrics are recorded on the hot path: `aeronet.bytes.read` on every socket read, `aeronet.events.processed` on every poll, and the HTTP/2 instruments about 20 times per request. They are batched before being exported:

- Counters are summed in memory, per name and labels, and exported at the next flush: one OTLP data point update and one DogStatsD line per counter, however often it was incremented.
- DogStatsD lines (counters at the flush, gauges, histograms, timings and sets when recorded) are buffered and sent several per datagram, one per line. A datagram is sent when the next line would make it exceed 8 KiB (`DogStatsD::kMaxDatagramSize`, the default `dogstatsd_buffer_size` of the Datadog Agent), and at the flush.

`SingleHttpServer` flushes its telemetry at each maintenance tick (at least every `pollInterval`), so a counter reaches the exporters at most one tick after it was incremented. The `HttpClient` flushes after each request. The custom metrics sent through `TelemetryContext::dogstatsdClient()` are buffered the same way. A `TelemetryContext` or a `DogStatsD` client of your own must call `flush()` periodically; their destructors flush too.

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
telemetry.flush();  // exports the summed counters, see Batching and flush
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
| `aeronet.http2.streams.closed_for_idle_timeout` | Counter | streams | none | Streams closed after waiting for their client without progress for `Http2Config::streamIdleTimeout`: a request body (answered `408`) or a response (reset with `CANCEL`). |
| `aeronet.http2.streams.rejected_orphaned_async_tasks` | Counter | streams | none | Streams refused with `REFUSED_STREAM` because the deferred work of closed streams still counts against `maxConcurrentStreams`. |
| `aeronet.http2.streams.rejected_malformed` | Counter | streams | none | Streams reset with `PROTOCOL_ERROR`, without response, because their request is malformed (RFC 9113 §8.1, §8.2, §8.3): its field section, pseudo-header fields, `content-length` or trailers, or a header block on a CONNECT stream. The other protocol errors of a stream appear in `aeronet.http2.streams.closed` only. |
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

When telemetry is disabled, each detailed HTTP/2 instrumentation point is a predictable null check and does not allocate. Instrument recording, span recording and DogStatsD datagram emission occur on the event-loop thread; OTLP metric and span exports run periodically on background threads. Counter increments only update an in-memory sum, and DogStatsD packs its lines into datagrams of up to 8 KiB (see [Batching and flush](#batching-and-flush)). Keep DogStatsD sockets local and non-blocking and configure OTLP export intervals appropriately. Existing unlabeled metric overloads keep their original call shape; only calls that supply labels construct and visit a `MetricLabels` span.

The implementation adds no counters or timestamps to `Http2Stream` or to the protocol handler's per-stream request state. Per-stream duration reuses the request start timestamp that already exists. `TelemetryContext` remains one pointer and `Http2Stream` remains 32 bytes on the project's 64-bit build; `Http2Connection` gains one optional pointer (8 bytes) for the telemetry destination.

### Built-in instrumentation

Without handler code, each server emits:

| Signal | Name | Content |
| --- | --- | --- |
| Span | `http.request` | One per sampled request, created once its head is received (whatever the number of reads), none for a rejected head, with `http.method`, `http.target`, `http.scheme` (`https` over TLS), `http.host`, and `http.status_code`. Its duration is the one of the request, from its head to its response. |
| Span | `aeronet.middleware` | One per middleware call, with its phase, scope, index, and whether it short-circuited, threw, or ran for a streaming route. |
| Counters | `aeronet.connections.accepted`, `aeronet.events.processed`, `aeronet.events.errors`, `aeronet.bytes.read` | Connection and event loop activity. `aeronet.events.errors` counts the failed polls, each of which stops its server (see [Stopping](../guides/server-lifecycle.md#stopping)). |
| Counters | `aeronet.connections.closed_for_*` | Connections closed by a timeout (`keep_alive`, `stalled_output` for a response that made no progress for `keepAliveTimeout`, `header_read_timeout`, `body_read_timeout`, `request_timeout`, `handshake_timeout`, `lingering_close_timeout`, `websocket_idle_timeout`, `websocket_close_timeout`, `http2_settings_timeout`, `http2_ping_timeout`), a drain (`drain`), or [`MSG_ZEROCOPY`](performance.md#msg_zerocopy) sends without progress (`zerocopy_stall`). |
| Counters | `aeronet.connections.lingering_closes` | HTTP/1.1 connections closed in stages, their client being possibly still sending (see [Lingering close](../guides/server-lifecycle.md#lingering-close)). |
| Counters | `aeronet.connections.aborted_with_zerocopy_in_flight` | Connections reset while some of their `MSG_ZEROCOPY` sends were in flight. |
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

Read [Health probes](health-probes.md), then use the [Kubernetes deployment guide](kubernetes.md) for ConfigMap and manifest examples.

## Tests

- OpenTelemetry export end to end, with one `http.request` span for a head received in several reads and its attributes (`OneSpanPerRequestWhateverTheReadsOfItsHead`): [tests/opentelemetry-e2e_test.cpp](../../tests/opentelemetry-e2e_test.cpp). The scheme over TLS: `HttpRequestViewTest.TraceSpanSchemeIsTheOneOfTheConnection` in [http-request-view_test.cpp](../../aeronet/http/test/http-request-view_test.cpp).
- Sampling before the span is created: [span-sampler_test.cpp](../../aeronet/objects/test/span-sampler_test.cpp) and `OpenTelemetryIntegration.SpanSampling` in [opentelemetry-integration_test.cpp](../../aeronet/objects/test/opentelemetry-integration_test.cpp).
- Span encoding and batching: [span-recorder_test.cpp](../../aeronet/objects/test/span-recorder_test.cpp) (see [Spans](#spans)).
- DogStatsD: [dogstatsd_test.cpp](../../aeronet/objects/test/dogstatsd_test.cpp).
- Counter batching: [counter-accumulator_test.cpp](../../aeronet/objects/test/counter-accumulator_test.cpp), the `DogStatsDCounters*` tests of [opentelemetry-integration_test.cpp](../../aeronet/objects/test/opentelemetry-integration_test.cpp), `HttpServerTelemetry.DogStatsDMetricsSentAtMaintenanceTick` in [tests/http-core_test.cpp](../../tests/http-core_test.cpp) and `HttpClientE2ETest.TelemetryCountersSentAfterEachRequest` in [http-client-core_test.cpp](../../aeronet/client/test/http-client-core_test.cpp).
- Access log: [access-log-writer_test.cpp](../../aeronet/server/test/access-log-writer_test.cpp).
- `reusedConnection` of the request metrics (`HttpKeepAlive.MetricsReportReusedConnection`): [tests/http-core_test.cpp](../../tests/http-core_test.cpp).
