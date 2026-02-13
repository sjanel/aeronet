#pragma once

#include <cstddef>
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

#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-decoder.hpp"
#include "aeronet/brotli-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#include "aeronet/zlib-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZSTD
#include "aeronet/zstd-decoder.hpp"
#include "aeronet/zstd-encoder.hpp"
#endif

namespace aeronet::internal {

// TODO: can we override all alloc / free calls from these decoders / encoders to use a shared arena or pool?

struct RequestDecompressionState {
#ifdef AERONET_ENABLE_BROTLI
  BrotliDecoder brotliDecoder;
#endif

#ifdef AERONET_ENABLE_ZLIB
  ZlibDecoder zlibDecoder;
#endif

#ifdef AERONET_ENABLE_ZSTD
  ZstdDecoder zstdDecoder;
#endif
};

struct ResponseCompressionState {
  ResponseCompressionState() noexcept = default;

  explicit ResponseCompressionState(const CompressionConfig& cfg);

  std::size_t encodeFull(Encoding encoding, std::string_view data, std::size_t availableCapacity, char* buf);

  // Initializes a new internally-owned encoder context and returns a pointer to it (reused across calls).
  EncoderContext* makeContext(Encoding encoding);

  // returns a pointer to an internally-owned encoder context for the given encoding, or nullptr if the encoding is not
  // supported.
  EncoderContext* context(Encoding encoding);

  EncodingSelector selector;
  const CompressionConfig* pCompressionConfig{nullptr};

#ifdef AERONET_ENABLE_BROTLI
  BrotliEncoder brotliEncoder;
#endif

#ifdef AERONET_ENABLE_ZLIB
  ZlibEncoder zlibEncoder;
#endif

#ifdef AERONET_ENABLE_ZSTD
  ZstdEncoder zstdEncoder;
#endif
};

struct RequestDecompressionResult {
  http::StatusCode status{http::StatusCodeOK};
  const char* message = nullptr;
};

class HttpCodec {
 public:
  static void TryCompressResponse(ResponseCompressionState& compressionState,
                                  const CompressionConfig& compressionConfig, Encoding encoding, HttpResponse& resp);

  /// Decompress request body for fixed-length requests (so they cannot contain any trailers).
  static RequestDecompressionResult MaybeDecompressRequestBody(RequestDecompressionState& decompressionState,
                                                               const DecompressionConfig& decompressionConfig,
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
  static RequestDecompressionResult DecompressChunkedBody(RequestDecompressionState& decompressionState,
                                                          const DecompressionConfig& decompressionConfig,
                                                          HttpRequest& request,
                                                          std::span<const std::string_view> compressedChunks,
                                                          std::size_t compressedSize, RawChars& bodyAndTrailersBuffer,
                                                          RawChars& tmpBuffer);
};

}  // namespace aeronet::internal
