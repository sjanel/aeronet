#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/decoder.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/stringconv.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-decoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZSTD
#include "aeronet/zstd-decoder.hpp"
#endif

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
  static void TryCompressResponse(ResponseCompressionState& compressionState,
                                  const CompressionConfig& compressionConfig, const HttpRequest& request,
                                  HttpResponse& resp);

  template <typename ParseTrailersFn>
  static RequestDecompressionResult MaybeDecompressRequestBody(const DecompressionConfig& decompressionConfig,
                                                               HttpRequest& request, RawChars& bodyAndTrailersBuffer,
                                                               std::size_t& trailerStartPos, RawChars& tmpBuffer,
                                                               RawChars& trailersScratch,
                                                               ParseTrailersFn&& parseTrailers) {
    auto& headersMap = request._headers;
    const auto encodingHeaderIt = headersMap.find(http::ContentEncoding);
    if (encodingHeaderIt == headersMap.end() || CaseInsensitiveEqual(encodingHeaderIt->second, http::identity)) {
      return {};
    }

    if (!decompressionConfig.enable) {
      return {};
    }

    const std::size_t originalCompressedSize = request.body().size();
    if (decompressionConfig.maxCompressedBytes != 0 &&
        originalCompressedSize > decompressionConfig.maxCompressedBytes) {
      return {.ok = false, .status = http::StatusCodePayloadTooLarge, .message = {}};
    }

    const std::string_view encodingStr = encodingHeaderIt->second;
    std::string_view src = request.body();
    RawChars* dst = &tmpBuffer;

    const auto contentLenIt = headersMap.find(http::ContentLength);

#if defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZSTD)
    const std::size_t maxPlainBytes = decompressionConfig.maxDecompressedBytes == 0
                                          ? std::numeric_limits<std::size_t>::max()
                                          : decompressionConfig.maxDecompressedBytes;

    bool useStreamingDecode = false;
    if (decompressionConfig.streamingDecompressionThresholdBytes > 0 && contentLenIt != headersMap.end()) {
      const std::string_view contentLenValue = contentLenIt->second;
      const std::size_t declaredLen = StringToIntegral<std::size_t>(contentLenValue);
      useStreamingDecode = declaredLen >= decompressionConfig.streamingDecompressionThresholdBytes;
    }

    const auto runDecoder = [&](Decoder& decoder) -> bool {
      if (!useStreamingDecode) {
        return decoder.decompressFull(src, maxPlainBytes, decompressionConfig.decoderChunkSize, *dst);
      }
      auto ctx = decoder.makeContext();
      if (!ctx) {
        return false;
      }
      if (src.empty()) {
        return ctx->decompressChunk(std::string_view{}, true, maxPlainBytes, decompressionConfig.decoderChunkSize,
                                    *dst);
      }
      std::size_t processed = 0;
      while (processed < src.size()) {
        const std::size_t remaining = src.size() - processed;
        const std::size_t chunkLen = std::min(decompressionConfig.decoderChunkSize, remaining);
        std::string_view chunk(src.data() + processed, chunkLen);
        processed += chunkLen;
        const bool lastChunk = processed == src.size();
        if (!ctx->decompressChunk(chunk, lastChunk, maxPlainBytes, decompressionConfig.decoderChunkSize, *dst)) {
          return false;
        }
      }
      return true;
    };
#endif

    // Preserve request trailers (HTTP/1 chunked only). Trailer bytes are stored at the end of the buffer.
    trailersScratch.clear();
    const std::size_t trailersSize = trailerStartPos > 0 ? bodyAndTrailersBuffer.size() - trailerStartPos : 0;
    if (trailersSize > 0) {
      trailersScratch.assign(bodyAndTrailersBuffer.data() + trailerStartPos, trailersSize);
    }

    // Decode in reverse order.
    const char* first = encodingStr.data();
    const char* last = first + encodingStr.size();
    while (first < last) {
      const char* encodingLast = last;
      while (encodingLast != first && http::IsHeaderWhitespace(*encodingLast)) {
        --encodingLast;
      }
      if (encodingLast == first) {
        break;
      }
      const char* comma = encodingLast - 1;
      while (comma != first && *comma != ',') {
        --comma;
      }
      if (comma == first) {
        --comma;
      }
      const char* encodingFirst = comma + 1;
      while (encodingFirst != encodingLast && http::IsHeaderWhitespace(*encodingFirst)) {
        ++encodingFirst;
      }
      if (encodingFirst == encodingLast) {
        return {.ok = false, .status = http::StatusCodeBadRequest, .message = "Malformed Content-Encoding"};
      }

      std::string_view encoding(encodingFirst, encodingLast);
      dst->clear();
      bool stageOk = false;
      if (CaseInsensitiveEqual(encoding, http::identity)) {
        last = comma;
        continue;
#ifdef AERONET_ENABLE_ZLIB
        // NOLINTNEXTLINE(readability-else-after-return)
      } else if (CaseInsensitiveEqual(encoding, http::gzip)) {
        ZlibDecoder decoder(/*isGzip=*/true);
        stageOk = runDecoder(decoder);
      } else if (CaseInsensitiveEqual(encoding, http::deflate)) {
        ZlibDecoder decoder(/*isGzip=*/false);
        stageOk = runDecoder(decoder);
#endif
#ifdef AERONET_ENABLE_ZSTD
      } else if (CaseInsensitiveEqual(encoding, http::zstd)) {
        ZstdDecoder decoder;
        stageOk = runDecoder(decoder);
#endif
#ifdef AERONET_ENABLE_BROTLI
      } else if (CaseInsensitiveEqual(encoding, http::br)) {
        BrotliDecoder decoder;
        stageOk = runDecoder(decoder);
#endif
      } else {
        return {.ok = false, .status = http::StatusCodeUnsupportedMediaType, .message = "Unsupported Content-Encoding"};
      }

      if (!stageOk) {
        return {.ok = false, .status = http::StatusCodeBadRequest, .message = "Decompression failed"};
      }

      if (decompressionConfig.maxExpansionRatio > 0.0 && originalCompressedSize > 0) {
        double ratio = static_cast<double>(dst->size()) / static_cast<double>(originalCompressedSize);
        if (ratio > decompressionConfig.maxExpansionRatio) {
          return {
              .ok = false, .status = http::StatusCodePayloadTooLarge, .message = "Decompression expansion too large"};
        }
      }

      src = *dst;
      dst = dst == &bodyAndTrailersBuffer ? &tmpBuffer : &bodyAndTrailersBuffer;
      last = comma;
    }

    if (src.data() == tmpBuffer.data()) {
      tmpBuffer.swap(bodyAndTrailersBuffer);
    }
    RawChars& buf = bodyAndTrailersBuffer;

    const std::size_t decompressedSizeNbChars = static_cast<std::size_t>(nchars(src.size()));
    buf.ensureAvailableCapacity(trailersScratch.size() + decompressedSizeNbChars);

    src = bodyAndTrailersBuffer;
    if (!trailersScratch.empty()) {
      trailerStartPos = buf.size();
      buf.unchecked_append(trailersScratch);
      (void)parseTrailers(request._trailers, buf.data(), buf.data() + trailerStartPos, buf.end());
    }

    std::string_view decompressedSizeStr(buf.end(), decompressedSizeNbChars);
    std::to_chars(buf.end(), buf.end() + decompressedSizeNbChars, src.size());
    buf.addSize(decompressedSizeNbChars);

    request._body = src;

    const std::string_view originalContentLenStr = contentLenIt != headersMap.end() ? contentLenIt->second : "";
    headersMap.erase(encodingHeaderIt);
    headersMap.insert_or_assign(http::ContentLength, decompressedSizeStr);
    headersMap.insert_or_assign(http::OriginalEncodingHeaderName, encodingStr);
    if (!originalContentLenStr.empty()) {
      headersMap.insert_or_assign(http::OriginalEncodedLengthHeaderName, originalContentLenStr);
    }

    return {};
  }

  static RequestDecompressionResult MaybeDecompressRequestBody(const DecompressionConfig& decompressionConfig,
                                                               HttpRequest& request, RawChars& bodyAndTrailersBuffer,
                                                               std::size_t& trailerStartPos, RawChars& tmpBuffer,
                                                               RawChars& trailersScratch) {
    return MaybeDecompressRequestBody(
        decompressionConfig, request, bodyAndTrailersBuffer, trailerStartPos, tmpBuffer, trailersScratch,
        [](auto& /*trailersMap*/, char* /*bufferBeg*/, char* /*first*/, char* /*last*/) { return true; });
  }
};

}  // namespace aeronet::internal
