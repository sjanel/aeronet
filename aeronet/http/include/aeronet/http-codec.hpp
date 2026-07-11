#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/encoder-result.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-codec-result.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-request-view.hpp"
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

// All decoders / encoders use custom allocators (BufferCache or ObjectArrayPool) to reuse memory across sessions.

struct DecompressionState {
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

struct CompressionState {
  CompressionState() noexcept = default;

  explicit CompressionState(const CompressionConfig& cfg);

  EncoderResult encodeFull(Encoding encoding, std::string_view data, std::size_t availableCapacity, char* buf);

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

class HttpCodec {
 public:
  // Try to compress the response body with the given encoding. Returns true if compression was applied, false if not
  // (either because the encoding is not supported, or because compression failed or did not meet config thresholds). On
  // true return, the response is modified in-place with the compressed body and appropriate headers. On false return,
  // the response is left unmodified.
  static CompressResponseResult TryCompressBody(CompressionState& compressionState, Encoding encoding,
                                                HttpMessage& msg);

  /// Decompress request body for fixed-length requests (so they cannot contain any trailers).
  static RequestDecompressionResult MaybeDecompressRequestBody(DecompressionState& decompressionState,
                                                               const DecompressionConfig& decompressionConfig,
                                                               HttpRequestView& request,
                                                               RawChars& bodyAndTrailersBuffer, RawChars& tmpBuffer);

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
  static RequestDecompressionResult DecompressChunkedBody(DecompressionState& decompressionState,
                                                          const DecompressionConfig& decompressionConfig,
                                                          HttpRequestView& request,
                                                          std::span<const std::string_view> compressedChunks,
                                                          std::size_t compressedSize, RawChars& bodyAndTrailersBuffer,
                                                          RawChars& tmpBuffer);

  /// Decompress a fully-contiguous compressed body according to the given Content-Encoding header
  /// value. Supports stacked encodings (e.g. "gzip, br") and identity codings. The decompressed bytes
  /// are written into outBuffer (tmpBuffer is scratch for the multi-stage ping-pong) and `outDecompressed`
  /// is set to a view of the result (a view into one of the two buffers, or `compressedBody` itself when
  /// only identity codings were present so nothing was decoded). No header mutation is performed: callers
  /// own the message and adjust headers themselves. Reuses the exact same decoders/codepath as the
  /// server's inbound request decompression. Returns http::StatusCodeOK on success.
  static RequestDecompressionResult DecompressFullBody(DecompressionState& decompressionState,
                                                       const DecompressionConfig& decompressionConfig,
                                                       std::string_view contentEncoding,
                                                       std::string_view compressedBody, RawChars& outBuffer,
                                                       RawChars& tmpBuffer, std::string_view& outDecompressed);
};

}  // namespace aeronet::internal
