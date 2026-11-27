#include "otlp-span-export.hpp"

#include <opentelemetry/exporters/otlp/otlp_environment.h>
#include <opentelemetry/exporters/otlp/otlp_http.h>
#include <opentelemetry/exporters/otlp/otlp_http_client.h>
#include <opentelemetry/exporters/otlp/otlp_populate_attribute_utils.h>
#include <opentelemetry/sdk/common/exporter_utils.h>
#include <opentelemetry/sdk/common/thread_instrumentation.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/version.h>

// clang-format off
#include <opentelemetry/exporters/otlp/protobuf_include_prefix.h>  // IWYU pragma: keep
#include <google/protobuf/arena.h>
#include <google/protobuf/message.h>
#include <google/protobuf/unknown_field_set.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.pb.h>
#include <opentelemetry/proto/common/v1/common.pb.h>
#include <opentelemetry/proto/resource/v1/resource.pb.h>
#include <opentelemetry/exporters/otlp/protobuf_include_suffix.h>  // IWYU pragma: keep
// clang-format on

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/log.hpp"
#include "aeronet/telemetry-config.hpp"
#include "span-recorder.hpp"

namespace aeronet::tracing::detail {

namespace {

namespace otlp_exporter = opentelemetry::exporter::otlp;

using opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest;
using opentelemetry::proto::collector::trace::v1::ExportTraceServiceResponse;
using opentelemetry::sdk::common::ExportResult;

// ExportTraceServiceRequest.resource_spans.
constexpr int kExportTraceServiceRequestResourceSpans = 1;

// Copies 'bytes' into 'field', a length-delimited unknown field, whose string keeps its capacity from batch to batch.
// Up to protobuf 27 at least, through its mutable string. Protobuf 36 has no mutable string (length_delimited() is an
// absl::string_view): only set_length_delimited(), taking a view (in protobuf 3.21 and 27, a std::string). A template:
// the branch of the other protobuf versions is not compiled.
template <class Field>
void AssignLengthDelimited(Field& field, std::string_view bytes) {
  if constexpr (requires { field.mutable_length_delimited()->assign(bytes.data(), bytes.size()); }) {
    field.mutable_length_delimited()->assign(bytes.data(), bytes.size());
  } else {
    using View = decltype(field.length_delimited());
    field.set_length_delimited(View(bytes.data(), bytes.size()));
  }
}

// The options of the OTLP HTTP span exporter of opentelemetry-cpp (OtlpHttpExporterOptions), read from the same
// environment variables, but the content type: always protobuf.
otlp_exporter::OtlpHttpClientOptions MakeOtlpHttpClientOptions(const TelemetryConfig& cfg) {
  std::string url;
  if (cfg.endpoint().empty()) {
    log::warn("OTLP endpoint is empty; using default endpoint from environment or SDK defaults");
    url = otlp_exporter::GetOtlpDefaultHttpTracesEndpoint();
  } else {
    url = cfg.endpoint();
    log::info("Initializing OTLP HTTP trace exporter with endpoint: {}", cfg.endpoint());
  }
  if (otlp_exporter::GetOtlpDefaultHttpTracesProtocol() == "http/json") {
    // The batches are given to the client as bytes (see OtlpHttpSpanSink::send()), which its JSON conversion skips.
    log::warn("OTLP traces protocol 'http/json' is not supported: traces are sent in 'http/protobuf'");
  }
  return {url,
          /*input_ssl_insecure_skip_verify=*/false,
          otlp_exporter::GetOtlpDefaultTracesSslCertificatePath(),
          otlp_exporter::GetOtlpDefaultTracesSslCertificateString(),
          otlp_exporter::GetOtlpDefaultTracesSslClientKeyPath(),
          otlp_exporter::GetOtlpDefaultTracesSslClientKeyString(),
          otlp_exporter::GetOtlpDefaultTracesSslClientCertificatePath(),
          otlp_exporter::GetOtlpDefaultTracesSslClientCertificateString(),
          otlp_exporter::GetOtlpDefaultTracesSslTlsMinVersion(),
          otlp_exporter::GetOtlpDefaultTracesSslTlsMaxVersion(),
          otlp_exporter::GetOtlpDefaultTracesSslTlsCipher(),
          otlp_exporter::GetOtlpDefaultTracesSslTlsCipherSuite(),
          otlp_exporter::HttpRequestContentType::kBinary,
          otlp_exporter::JsonBytesMappingKind::kHexId,
          otlp_exporter::GetOtlpDefaultTracesCompression(),
          /*input_use_json_name=*/false,
          /*input_console_debug=*/false,
          otlp_exporter::GetOtlpDefaultTracesTimeout(),
          MakeOtlpHeaders(cfg),
          otlp_exporter::GetOtlpDefaultTracesRetryMaxAttempts(),
          otlp_exporter::GetOtlpDefaultTracesRetryInitialBackoff(),
          otlp_exporter::GetOtlpDefaultTracesRetryMaxBackoff(),
          otlp_exporter::GetOtlpDefaultTracesRetryBackoffMultiplier(),
          std::shared_ptr<opentelemetry::sdk::common::ThreadInstrumentation>{nullptr}};
}

class OtlpHttpSpanSink final : public SpanBatchSink {
 public:
  explicit OtlpHttpSpanSink(const TelemetryConfig& cfg)
      : _client(MakeOtlpHttpClientOptions(cfg)), _pResourceSpans(AddResourceSpansField(_request)) {}

  void send(std::string_view resourceSpans, uint32_t nbSpans) noexcept override {
    // The batch is the only ResourceSpans of the request, an unknown field of the generated message, which its binary
    // serialization writes as it is, into the body of the HTTP request.
    AssignLengthDelimited(*_pResourceSpans, resourceSpans);
#if OPENTELEMETRY_VERSION_MAJOR > 1 || OPENTELEMETRY_VERSION_MINOR >= 28
    // From opentelemetry-cpp 1.28, the client parses the response in a message of the caller, on an arena it keeps
    // until the end of the request.
    auto arena = std::make_unique<google::protobuf::Arena>();
    auto* pResponse = google::protobuf::Arena::Create<ExportTraceServiceResponse>(arena.get());
    const ExportResult result = _client.Export(
        _request, std::move(arena), pResponse,
        [nbSpans](ExportResult exportResult, google::protobuf::Message* pMessage) {
          if (exportResult == ExportResult::kSuccess && pMessage != nullptr) {
            const auto& partialSuccess = static_cast<const ExportTraceServiceResponse*>(pMessage)->partial_success();
            if (partialSuccess.rejected_spans() != 0 || !partialSuccess.error_message().empty()) {
              log::error("OTLP trace export partial success: {} span(s) of {} rejected: '{}'",
                         partialSuccess.rejected_spans(), nbSpans, partialSuccess.error_message());
            }
          }
          return true;
        },
        /*max_running_requests=*/0);
#else
    const ExportResult result = _client.Export(_request);
#endif
    if (result != ExportResult::kSuccess) {
      log::error("OTLP trace export of {} span(s) failed: {}", nbSpans, static_cast<int>(result));
    }
  }

  void shutdown() noexcept override { _client.Shutdown(); }

 private:
  static google::protobuf::UnknownField* AddResourceSpansField(ExportTraceServiceRequest& request) {
    google::protobuf::UnknownFieldSet& unknownFields =
        *ExportTraceServiceRequest::GetReflection()->MutableUnknownFields(&request);
    unknownFields.AddLengthDelimited(kExportTraceServiceRequestResourceSpans, std::string{});
    return unknownFields.mutable_field(0);
  }

  otlp_exporter::OtlpHttpClient _client;
  ExportTraceServiceRequest _request;
  // The unknown field of _request holding the bytes of the batch sent.
  google::protobuf::UnknownField* _pResourceSpans;
};

}  // namespace

otlp_exporter::OtlpHeaders MakeOtlpHeaders(const TelemetryConfig& cfg) {
  otlp_exporter::OtlpHeaders headers;
  // The HTTP headers are stored as 'name:value' strings.
  for (auto header : cfg.httpHeadersRange()) {
    const auto colonPos = header.find(':');
    headers.emplace(std::string(header.substr(0, colonPos)), std::string(header.substr(colonPos + 1)));
  }
  return headers;
}

SpanBatchHeader MakeOtlpSpanBatchHeader(const opentelemetry::sdk::resource::Resource& resource) {
  // As OtlpRecordableUtils::PopulateRequest() fills the ResourceSpans and ScopeSpans of the spans of a tracer named
  // "aeronet", of the version of aeronet, without attributes nor schema URL.
  opentelemetry::proto::resource::v1::Resource resourceMessage;
  otlp_exporter::OtlpPopulateAttributeUtils::PopulateAttribute(&resourceMessage, resource);
  opentelemetry::proto::common::v1::InstrumentationScope scopeMessage;
  scopeMessage.set_name("aeronet");
  scopeMessage.set_version(AERONET_VERSION_STR);
  return {resourceMessage.SerializeAsString(), resource.GetSchemaURL(), scopeMessage.SerializeAsString(),
          std::string_view{}};
}

std::unique_ptr<SpanBatchSink> MakeOtlpHttpSpanSink(const TelemetryConfig& cfg) {
  return std::make_unique<OtlpHttpSpanSink>(cfg);
}

}  // namespace aeronet::tracing::detail
