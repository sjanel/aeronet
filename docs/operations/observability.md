# Observability and logging

aeronet provides opt-in logging and OpenTelemetry integration so applications can expose protocol and request behavior without embedding a global logging policy in the library.

## Logging

The logging module integrates with spdlog when enabled. Configure the sink, level, and format for the environment that runs your service; include enough request context to diagnose failures without placing sensitive payloads or credentials in logs.

See [Logging](../FEATURES.md#logging) for the supported configuration and structured access-log behavior.

## OpenTelemetry

Enable OpenTelemetry at build time when the application needs traces or metrics. aeronet exposes built-in instrumentation and configuration hooks while leaving exporter and deployment choices explicit.

The [OpenTelemetry reference](../FEATURES.md#opentelemetry-integration) describes the instrumentation surface, metric configuration, collector testing, and dependency requirements.

## Health probes and deployment

The server includes Kubernetes-style probe support. Keep a probe endpoint lightweight, make readiness reflect dependencies that actually gate traffic, and consider a dedicated listener when probe availability must be isolated from application load.

Read [Built-in Kubernetes-style probes](../FEATURES.md#built-in-kubernetes-style-probes), then use the [Kubernetes deployment guide](../kubernetes-examples.md) for ConfigMap and manifest examples.
