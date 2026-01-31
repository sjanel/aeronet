#include "aeronet/http-response-prefinalize.hpp"

#include "aeronet/encoding.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/template-constants.hpp"

namespace aeronet::internal {

void PrefinalizeHttpResponse(const HttpRequest& request, HttpResponse& response, bool isHead,
                             ResponseCompressionState& compressionState, const HttpServerConfig& serverConfig) {
  if (isHead) {
    return;
  }

  if (response.status() == http::StatusCodeNotFound && !response.hasBody()) {
    response.bodyStatic(k404NotFoundTemplate2, http::ContentTypeTextHtml);
  }

  const Encoding encoding = request.responsePossibleEncoding();

  if (response.hasBodyInMemory() && encoding != Encoding::none) {
    HttpCodec::TryCompressResponse(compressionState, serverConfig.compression, encoding, response);
  }
}

}  // namespace aeronet::internal
