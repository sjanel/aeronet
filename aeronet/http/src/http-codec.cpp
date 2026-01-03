#include "aeronet/http-codec.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::internal {

void HttpCodec::TryCompressResponse(ResponseCompressionState& compressionState,
                                    const CompressionConfig& compressionConfig, const HttpRequest& request,
                                    HttpResponse& resp) {
  if (resp.body().size() < compressionConfig.minBytes) {
    return;
  }
  const std::string_view encHeader = request.headerValueOrEmpty(http::AcceptEncoding);
  auto [encoding, reject] = compressionState.selector.negotiateAcceptEncoding(encHeader);
  // If the client explicitly forbids identity (identity;q=0) and we have no acceptable
  // alternative encodings to offer, emit a 406 per RFC 9110 Section 12.5.3 guidance.
  if (reject) {
    resp.status(http::StatusCodeNotAcceptable).body("No acceptable content-coding available");
  }
  if (encoding == Encoding::none) {
    return;
  }

  if (!compressionConfig.contentTypeAllowList.empty()) {
    std::string_view contentType = request.headerValueOrEmpty(http::ContentType);
    if (!compressionConfig.contentTypeAllowList.containsCI(contentType)) {
      return;
    }
  }

  if (resp.headerValue(http::ContentEncoding)) {
    return;
  }

  // First, write the needed headers.
  resp.addHeader(http::ContentEncoding, GetEncodingStr(encoding));
  if (compressionConfig.addVaryHeader) {
    resp.appendHeaderValue(http::Vary, http::AcceptEncoding);
  }

  auto& encoder = compressionState.encoders[static_cast<std::size_t>(encoding)];

  auto* pExternPayload = resp.externPayloadPtr();
  if (pExternPayload != nullptr) {
    const auto externView = pExternPayload->view();
    const auto externTrailers = resp.externalTrailers(*pExternPayload);
    const std::string_view externBody(externView.data(), externView.size() - externTrailers.size());
    const auto oldSize = resp._data.size();

    encoder->encodeFull(externTrailers.size(), externBody, resp._data);

    const std::size_t compressedBodyLen = resp._data.size() - oldSize;

    if (!externTrailers.empty()) {
      resp._data.append(externTrailers);
      resp._trailerPos = compressedBodyLen;
    }

    resp._payloadVariant = {};
  } else {
    const auto internalTrailers = resp.internalTrailers();
    RawChars out;
    encoder->encodeFull(internalTrailers.size(), resp.body(), out);

    if (resp._trailerPos != 0) {
      resp._trailerPos = out.size();
      out.append(internalTrailers);
    }

    resp._data.setSize(resp._data.size() - resp.internalBodyAndTrailersLen());
    resp._payloadVariant = HttpPayload(std::move(out));
  }
}

}  // namespace aeronet::internal
