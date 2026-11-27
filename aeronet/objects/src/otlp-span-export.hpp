#pragma once

#include <opentelemetry/exporters/otlp/otlp_environment.h>
#include <opentelemetry/sdk/resource/resource.h>

#include <memory>

#include "aeronet/telemetry-config.hpp"
#include "span-recorder.hpp"

// The OTLP export of the spans of SpanRecorder, through opentelemetry-cpp: its resource, its OTLP HTTP client and the
// options of the latter (configuration and OTEL_EXPORTER_OTLP_* environment variables), as its OTLP HTTP span
// exporter would. Not its trace SDK: aeronet encodes the spans itself.
namespace aeronet::tracing::detail {

// The HTTP headers of the OTLP exports of 'cfg'.
[[nodiscard]] opentelemetry::exporter::otlp::OtlpHeaders MakeOtlpHeaders(const TelemetryConfig& cfg);

// The header of the batches of spans: 'resource', and the instrumentation scope of aeronet (name and version), encoded
// as the OTLP exporters of opentelemetry-cpp do.
[[nodiscard]] SpanBatchHeader MakeOtlpSpanBatchHeader(const opentelemetry::sdk::resource::Resource& resource);

// A sink posting each batch to the OTLP/HTTP traces endpoint of 'cfg' (or of the environment when empty), always in
// protobuf: the JSON protocol, if asked by the environment, is ignored with a warning.
[[nodiscard]] std::unique_ptr<SpanBatchSink> MakeOtlpHttpSpanSink(const TelemetryConfig& cfg);

}  // namespace aeronet::tracing::detail
