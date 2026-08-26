# Observability and logging

`aeronet` provides opt-in logging and OpenTelemetry integration so applications can expose protocol and request behavior without embedding a global logging policy in the library.

## Logging

The logging module integrates with spdlog when enabled. Configure the sink, level, and format for the environment that runs your service; include enough request context to diagnose failures without placing sensitive payloads or credentials in logs.

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

`useForwardedFor` is a trust-boundary setting, not a general convenience switch. Do not log request bodies or authorization credentials. OpenTelemetry requires `AERONET_ENABLE_OPENTELEMETRY=ON`; DogStatsD can be enabled separately with a Unix-domain socket, tags, namespace, and an optional metric-only configuration.

## OpenTelemetry

Enable OpenTelemetry at build time when the application needs traces or metrics. aeronet exposes built-in instrumentation and configuration hooks while leaving exporter and deployment choices explicit.

The [OpenTelemetry reference](../FEATURES.md#opentelemetry-integration) describes the instrumentation surface, metric configuration, collector testing, and dependency requirements.

## Health probes and deployment

The server includes Kubernetes-style probe support. Keep a probe endpoint lightweight, make readiness reflect dependencies that actually gate traffic, and consider a dedicated listener when probe availability must be isolated from application load.

Read [Built-in Kubernetes-style probes](../FEATURES.md#built-in-kubernetes-style-probes), then use the [Kubernetes deployment guide](../kubernetes-examples.md) for ConfigMap and manifest examples.
