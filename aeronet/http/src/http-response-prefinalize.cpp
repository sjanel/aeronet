#include "aeronet/http-response-prefinalize.hpp"

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
    response.body(k404NotFoundTemplate2, http::ContentTypeTextHtml);
  }

  if (response.hasBodyInMemory()) {
    HttpCodec::TryCompressResponse(compressionState, serverConfig.compression,
                                   request.headerValueOrEmpty(http::AcceptEncoding), response);
  }
}

}  // namespace aeronet::internal
