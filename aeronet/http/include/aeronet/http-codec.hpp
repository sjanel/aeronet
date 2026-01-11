#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
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
  http::StatusCode status{http::StatusCodeOK};
  const char* message = nullptr;
};

class HttpCodec {
 public:
  static void TryCompressResponse(ResponseCompressionState& compressionState,
                                  const CompressionConfig& compressionConfig, const HttpRequest& request,
                                  HttpResponse& resp);

  /// Decompress request body for fixed-length requests (so they cannot contain any trailers).
  static RequestDecompressionResult MaybeDecompressRequestBody(const DecompressionConfig& decompressionConfig,
                                                               HttpRequest& request, RawChars& bodyAndTrailersBuffer,
                                                               RawChars& tmpBuffer);

  /// Check if decompression will be applied for the given request based on config and headers.
  /// This can be called before body decoding to determine the optimal path.
  /// Returns http::StatusCodeOK if decompression will be applied,
  /// http::StatusCodeNotModified if no decompression is needed,
  /// or http::StatusCodeBadRequest if the request contains invalid encoding.
  static http::StatusCode WillDecompress(const DecompressionConfig& decompressionConfig,
                                         const HeadersViewMap& headersMap);

  /// Decompress chunked body directly from source chunks (avoids intermediate copy).
  /// The chunks span points to non-contiguous compressed data (from chunked transfer).
  /// Decompressed output goes to bodyAndTrailersBuffer.
  /// Returns error result on failure, or empty result on success.
  static RequestDecompressionResult DecompressChunkedBody(const DecompressionConfig& decompressionConfig,
                                                          HttpRequest& request,
                                                          std::span<const std::string_view> compressedChunks,
                                                          std::size_t compressedSize, RawChars& bodyAndTrailersBuffer,
                                                          RawChars& tmpBuffer);
};

}  // namespace aeronet::internal
