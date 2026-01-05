#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::internal {

struct ResponseCompressionState {
  ResponseCompressionState() noexcept = default;

  explicit ResponseCompressionState(const CompressionConfig& cfg) : selector(cfg) {}

  // Pre-allocated encoders (one per supported format), -1 to remove identity which is last (no encoding).
  // Index corresponds to static_cast<size_t>(Encoding).
  std::array<std::unique_ptr<Encoder>, kNbContentEncodings - 1> encoders;
  EncodingSelector selector;
};

struct RequestDecompressionResult {
  bool ok{true};
  http::StatusCode status{http::StatusCodeOK};
  const char* message = nullptr;
};

class HttpCodec {
 public:
  using ParseTrailersFn = std::function<void(HeadersViewMap&, char*, char*, char*)>;

  static void TryCompressResponse(ResponseCompressionState& compressionState,
                                  const CompressionConfig& compressionConfig, const HttpRequest& request,
                                  HttpResponse& resp);

  static RequestDecompressionResult MaybeDecompressRequestBody(const DecompressionConfig& decompressionConfig,
                                                               HttpRequest& request, RawChars& bodyAndTrailersBuffer,
                                                               std::size_t& trailerStartPos, RawChars& tmpBuffer,
                                                               RawChars& trailersScratch,
                                                               const ParseTrailersFn& parseTrailers = {});
};

}  // namespace aeronet::internal
