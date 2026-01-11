#include "aeronet/http-codec.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/compression-config.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
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

namespace {

// Helper to iterate Content-Encoding header values in reverse order.
class EncodingReverseIterator {
 public:
  explicit EncodingReverseIterator(std::string_view headerValue)
      : first(headerValue.data()), last(first + headerValue.size()) {}

  // Returns true if there is another encoding to read.
  [[nodiscard]] bool hasNext() const noexcept { return first < last; }

  // Returns the next encoding token (trimmed), or empty string_view if malformed.
  std::string_view next() {
    // Header values are already trimmed, so we should not start with an OWS char.
    assert(!http::IsHeaderWhitespace(*(last - 1)));

    // Let's find the next separator to the left, that is the next comma, or OWS, or last.
    const char* nextSep = last - 1;
    while (nextSep >= first && *nextSep != ',' && !http::IsHeaderWhitespace(*nextSep)) {
      --nextSep;
    }

    std::string_view encoding(nextSep + 1, last);
    if (encoding.empty()) {
      // empty token forbidden
      return encoding;
    }

    // go to next non-OWS/comma char to the left and reject multiple commas in a row
    bool seenComma = false;
    while (nextSep >= first && (http::IsHeaderWhitespace(*nextSep) || *nextSep == ',')) {
      if (*nextSep == ',') {
        if (seenComma) {
          // two commas in a row, malformed
          encoding = {};
          return encoding;
        }
        seenComma = true;
      }
      --nextSep;
    }
    last = nextSep + 1;

    return encoding;
  }

 private:
  const char* first;
  const char* last;
};

inline std::string_view FinalizeDecompressedBody(HeadersViewMap& headersMap, HeadersViewMap::iterator encodingHeaderIt,
                                                 std::string_view src, RawChars& buf) {
  const std::size_t decompressedSizeNbChars = static_cast<std::size_t>(nchars(src.size()));
  buf.ensureAvailableCapacity(decompressedSizeNbChars);

  // Set the new decompressed body AFTER reallocating the buffer above.
  std::string_view body = buf;

  std::string_view decompressedSizeStr(buf.end(), decompressedSizeNbChars);
  [[maybe_unused]] const auto [ptr, errc] = std::to_chars(buf.end(), buf.end() + decompressedSizeNbChars, src.size());
  assert(errc == std::errc{} && ptr == buf.end() + decompressedSizeNbChars);
  buf.addSize(decompressedSizeNbChars);

  // Update Content encoding and Content-Length headers, and set special aeronet headers containing orignal values.
  const std::string_view encodingStr = encodingHeaderIt->second;
  const auto contentLenIt = headersMap.find(http::ContentLength);
  const std::string_view originalContentLenStr = contentLenIt != headersMap.end() ? contentLenIt->second : "";
  headersMap.erase(encodingHeaderIt);
  headersMap.insert_or_assign(http::ContentLength, decompressedSizeStr);
  headersMap.insert_or_assign(http::OriginalEncodingHeaderName, encodingStr);
  if (!originalContentLenStr.empty()) {
    headersMap.insert_or_assign(http::OriginalEncodedLengthHeaderName, originalContentLenStr);
  }

  return body;
}

inline RequestDecompressionResult DualBufferDecodeLoop(auto&& runDecoder, double maxExpansionRatio,
                                                       std::string_view& src, HeadersViewMap::iterator encodingHeaderIt,
                                                       std::size_t compressedSize, RawChars& bodyAndTrailersBuffer,
                                                       RawChars& tmpBuffer) {
  RawChars* dst = &tmpBuffer;

  http::StatusCode decompressStatus = http::StatusCodeNotModified;

  // Decode in reverse order, algorithm by algorithm.
  // For the first stage, we read from chunks. For subsequent stages, we read from the previous output buffer.
  for (EncodingReverseIterator encIt(encodingHeaderIt->second); encIt.hasNext();) {
    auto encoding = encIt.next();
    if (encoding.empty()) {
      return {.status = http::StatusCodeBadRequest, .message = "Malformed Content-Encoding"};
    }

    bool stageOk = false;
    if (CaseInsensitiveEqual(encoding, http::identity)) {
      continue;
#ifdef AERONET_ENABLE_ZLIB
      // NOLINTNEXTLINE(readability-else-after-return)
    } else if (CaseInsensitiveEqual(encoding, http::gzip)) {
      ZlibDecoder decoder(/*isGzip=*/true);
      stageOk = runDecoder(decoder, *dst);
    } else if (CaseInsensitiveEqual(encoding, http::deflate)) {
      ZlibDecoder decoder(/*isGzip=*/false);
      stageOk = runDecoder(decoder, *dst);
#endif
#ifdef AERONET_ENABLE_ZSTD
    } else if (CaseInsensitiveEqual(encoding, http::zstd)) {
      ZstdDecoder decoder;
      stageOk = runDecoder(decoder, *dst);
#endif
#ifdef AERONET_ENABLE_BROTLI
    } else if (CaseInsensitiveEqual(encoding, http::br)) {
      BrotliDecoder decoder;
      stageOk = runDecoder(decoder, *dst);
#endif
    } else {
      return {.status = http::StatusCodeUnsupportedMediaType, .message = "Unsupported Content-Encoding"};
    }

    if (!stageOk) {
      return {.status = http::StatusCodeBadRequest, .message = "Decompression failed"};
    }

    if (maxExpansionRatio > 0.0) {
      const double ratio = static_cast<double>(dst->size()) / static_cast<double>(compressedSize);
      if (ratio > maxExpansionRatio) {
        return {.status = http::StatusCodePayloadTooLarge, .message = "Decompression expansion too large"};
      }
    }

    decompressStatus = http::StatusCodeOK;

    src = *dst;
    dst = dst == &bodyAndTrailersBuffer ? &tmpBuffer : &bodyAndTrailersBuffer;
  }

  if (src.data() == tmpBuffer.data()) {
    // make sure the final result is in bodyAndTrailersBuffer
    tmpBuffer.swap(bodyAndTrailersBuffer);
  }

  return {.status = decompressStatus, .message = nullptr};
}

inline bool UseStreamingDecompression(const HeadersViewMap& headersMap,
                                      std::size_t streamingDecompressionThresholdBytes) {
  if (streamingDecompressionThresholdBytes > 0) {
    const auto contentLenIt = headersMap.find(http::ContentLength);
    if (contentLenIt != headersMap.end()) {
      const std::string_view contentLenValue = contentLenIt->second;
      const std::size_t declaredLen = StringToIntegral<std::size_t>(contentLenValue);

      return declaredLen >= streamingDecompressionThresholdBytes;
    }
  }
  return false;
}

}  // namespace

void HttpCodec::TryCompressResponse(ResponseCompressionState& compressionState,
                                    const CompressionConfig& compressionConfig, const HttpRequest& request,
                                    HttpResponse& resp) {
  if (resp.body().size() < compressionConfig.minBytes) {
    return;
  }
  const std::string_view encHeader = request.headerValueOrEmpty(http::AcceptEncoding);
  const auto [encoding, reject] = compressionState.selector.negotiateAcceptEncoding(encHeader);
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
  resp.headerAddLine(http::ContentEncoding, GetEncodingStr(encoding));
  if (compressionConfig.addVaryHeader) {
    resp.headerAppendValue(http::Vary, http::AcceptEncoding);
  }

  auto& encoder = compressionState.encoders[static_cast<std::size_t>(encoding)];

  auto* pExternPayload = resp.externPayloadPtr();
  if (pExternPayload != nullptr) {
    const auto externView = pExternPayload->view();
    const auto externTrailers = resp.externalTrailers(*pExternPayload);
    const std::string_view externBody(externView.data(), externView.size() - externTrailers.size());

    encoder->encodeFull(externTrailers.size(), externBody, resp._data);

    if (!externTrailers.empty()) {
      resp._data.append(externTrailers);
      resp._trailerLen = externTrailers.size();
    }

    resp._payloadVariant = {};
  } else {
    const auto internalTrailers = resp.internalTrailers();
    RawChars out;
    encoder->encodeFull(internalTrailers.size(), resp.body(), out);

    assert(out.availableCapacity() >= internalTrailers.size());
    out.unchecked_append(internalTrailers);

    resp._data.setSize(resp._data.size() - resp.internalBodyAndTrailersLen());
    resp._payloadVariant = HttpPayload(std::move(out));
  }
}

RequestDecompressionResult HttpCodec::MaybeDecompressRequestBody(const DecompressionConfig& decompressionConfig,
                                                                 HttpRequest& request, RawChars& bodyAndTrailersBuffer,
                                                                 RawChars& tmpBuffer) {
  if (!decompressionConfig.enable) {
    return {};
  }

  auto& headersMap = request._headers;
  const auto encodingHeaderIt = headersMap.find(http::ContentEncoding);
  if (encodingHeaderIt == headersMap.end()) {
    return {};
  }

  const std::size_t compressedSize = request.body().size();
  assert(compressedSize > 0);
  if (decompressionConfig.maxCompressedBytes != 0 && compressedSize > decompressionConfig.maxCompressedBytes) {
    return {.status = http::StatusCodePayloadTooLarge, .message = "Payload too large"};
  }

  const std::string_view encodingStr = encodingHeaderIt->second;
  if (encodingStr.empty()) {
    // Strict RFC compliance: empty Content-Encoding header is malformed.
    return {.status = http::StatusCodeBadRequest, .message = "Malformed Content-Encoding"};
  }

  std::string_view src = request.body();

  RequestDecompressionResult res = DualBufferDecodeLoop(
      [useStreamingDecode =
           UseStreamingDecompression(headersMap, decompressionConfig.streamingDecompressionThresholdBytes),
       &src, &decompressionConfig](auto& decoder, RawChars& dst) {
        dst.clear();
        if (!useStreamingDecode) {
          return decoder.decompressFull(src, decompressionConfig.maxDecompressedBytes,
                                        decompressionConfig.decoderChunkSize, dst);
        }
        auto ctx = decoder.makeContext();
        // The decompress body function cannot be called without body
        assert(!src.empty());
        for (std::size_t processed = 0; processed < src.size();) {
          const std::size_t remaining = src.size() - processed;
          const std::size_t chunkLen = std::min(decompressionConfig.decoderChunkSize, remaining);
          const std::string_view chunk(src.data() + processed, chunkLen);
          processed += chunkLen;
          const bool lastChunk = processed == src.size();
          if (!ctx.decompressChunk(chunk, lastChunk, decompressionConfig.maxDecompressedBytes,
                                   decompressionConfig.decoderChunkSize, dst)) {
            return false;
          }
        }
        return true;
      },
      decompressionConfig.maxExpansionRatio, src, encodingHeaderIt, compressedSize, bodyAndTrailersBuffer, tmpBuffer);

  if (res.status != http::StatusCodeOK) {
    if (res.status == http::StatusCodeNotModified) {
      // No decoding was actually performed (only identity codings)
      res.status = http::StatusCodeOK;
    }
    return res;
  }

  // at this point, src points to the final decompressed data buffer (either tmpBuffer or bodyAndTrailersBuffer)

  request._body = FinalizeDecompressedBody(headersMap, encodingHeaderIt, src, bodyAndTrailersBuffer);

  return {};
}

http::StatusCode HttpCodec::WillDecompress(const DecompressionConfig& decompressionConfig,
                                           const HeadersViewMap& headersMap) {
  if (!decompressionConfig.enable) {
    return http::StatusCodeNotModified;
  }
  const auto encodingHeaderIt = headersMap.find(http::ContentEncoding);
  if (encodingHeaderIt == headersMap.end()) {
    return http::StatusCodeNotModified;
  }
  const std::string_view encodingStr = encodingHeaderIt->second;
  if (encodingStr.empty()) {
    return http::StatusCodeBadRequest;
  }

  // Parse the encoding string to check for non-identity encodings
  EncodingReverseIterator encIt(encodingHeaderIt->second);
  assert(encIt.hasNext());
  do {
    auto encoding = encIt.next();
    if (encoding.empty()) {
      return http::StatusCodeBadRequest;
    }
    if (!CaseInsensitiveEqual(encoding, http::identity)) {
      return http::StatusCodeOK;  // Found a non-identity encoding
    }
  } while (encIt.hasNext());

  // Only identity
  return http::StatusCodeNotModified;
}

RequestDecompressionResult HttpCodec::DecompressChunkedBody(const DecompressionConfig& decompressionConfig,
                                                            HttpRequest& request,
                                                            std::span<const std::string_view> compressedChunks,
                                                            std::size_t compressedSize, RawChars& bodyAndTrailersBuffer,
                                                            RawChars& tmpBuffer) {
  auto& headersMap = request._headers;
  const auto encodingHeaderIt = headersMap.find(http::ContentEncoding);
  assert(encodingHeaderIt != headersMap.end());  // this shouldn't happen, WillDecompress was checked before
  assert(decompressionConfig.enable);

  std::string_view src;

  RequestDecompressionResult res = DualBufferDecodeLoop(
      [&src, compressedChunks, maxPlainBytes = decompressionConfig.maxDecompressedBytes,
       decoderChunkSize = decompressionConfig.decoderChunkSize,
       firstStage = true](auto& decoder, RawChars& dst) mutable {
        dst.clear();

        if (firstStage) {
          firstStage = false;
          auto ctx = decoder.makeContext();
          for (std::size_t idx = 0; idx < compressedChunks.size(); ++idx) {
            const bool lastChunk = (idx == compressedChunks.size() - 1);
            if (!ctx.decompressChunk(compressedChunks[idx], lastChunk, maxPlainBytes, decoderChunkSize, dst)) {
              return false;
            }
          }
          return true;
        }
        return decoder.decompressFull(src, maxPlainBytes, decoderChunkSize, dst);
      },
      decompressionConfig.maxExpansionRatio, src, encodingHeaderIt, compressedSize, bodyAndTrailersBuffer, tmpBuffer);
  if (res.status != http::StatusCodeOK) {
    return res;
  }

  request._body = FinalizeDecompressedBody(headersMap, encodingHeaderIt, src, bodyAndTrailersBuffer);

  return {};
}

}  // namespace aeronet::internal
