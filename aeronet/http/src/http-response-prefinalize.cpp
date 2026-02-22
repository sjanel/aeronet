#include "aeronet/http-response-prefinalize.hpp"

#include <cassert>

#include "aeronet/encoding.hpp"
#include "aeronet/http-codec-result.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/template-constants.hpp"
#include "aeronet/tracing/tracer.hpp"

namespace aeronet::internal {

void PrefinalizeHttpResponse(const HttpRequest& request, HttpResponse& response, bool isHead,
                             ResponseCompressionState& compressionState, tracing::TelemetryContext& telemetryContext) {
  if (isHead) {
    return;
  }

  if (response.status() == http::StatusCodeNotFound && !response.hasBody()) {
    response.bodyStatic(k404NotFoundTemplate2, http::ContentTypeTextHtml);
  }

  const Encoding encoding = request.responsePossibleEncoding();

  if (response.hasBodyInMemory() && encoding != Encoding::none) {
    CompressResponseResult result = HttpCodec::TryCompressResponse(compressionState, encoding, response);

    switch (result) {
      case internal::CompressResponseResult::uncompressed:
        break;
      case internal::CompressResponseResult::compressed:
        telemetryContext.counterAdd("aeronet.http_responses.compression.total", 1);
        break;
      case internal::CompressResponseResult::exceedsMaxRatio:
        telemetryContext.counterAdd("aeronet.http_responses.compression.exceeds_max_ratio_total", 1);
        break;
      default:
        assert(result == internal::CompressResponseResult::error);
        telemetryContext.counterAdd("aeronet.http_responses.compression.errors_total", 1);
        break;
    }
  }
}

}  // namespace aeronet::internal
