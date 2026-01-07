#include "aeronet/http-codec.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
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
                                                                 std::size_t& trailerStartPos, RawChars& tmpBuffer,
                                                                 RawChars& trailersScratch) {
  if (!decompressionConfig.enable) {
    return {};
  }

  auto& headersMap = request._headers;
  const auto encodingHeaderIt = headersMap.find(http::ContentEncoding);
  if (encodingHeaderIt == headersMap.end()) {
    return {};
  }

  const std::size_t originalCompressedSize = request.body().size();
  assert(originalCompressedSize > 0);
  if (decompressionConfig.maxCompressedBytes != 0 && originalCompressedSize > decompressionConfig.maxCompressedBytes) {
    return {.status = http::StatusCodePayloadTooLarge, .message = "Payload too large"};
  }

  const std::string_view encodingStr = encodingHeaderIt->second;
  if (encodingStr.empty()) {
    // Strict RFC compliance: empty Content-Encoding header is malformed.
    return {.status = http::StatusCodeBadRequest, .message = "Malformed Content-Encoding"};
  }

  std::string_view src = request.body();
  RawChars* dst = &tmpBuffer;

  const auto contentLenIt = headersMap.find(http::ContentLength);

#if defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZSTD)
  bool useStreamingDecode = false;
  if (decompressionConfig.streamingDecompressionThresholdBytes > 0 && contentLenIt != headersMap.end()) {
    const std::string_view contentLenValue = contentLenIt->second;
    const std::size_t declaredLen = StringToIntegral<std::size_t>(contentLenValue);

    useStreamingDecode = declaredLen >= decompressionConfig.streamingDecompressionThresholdBytes;
  }

  const auto runDecoder = [&, maxPlainBytes = decompressionConfig.maxDecompressedBytes](auto& decoder) {
    dst->clear();
    if (!useStreamingDecode) {
      return decoder.decompressFull(src, maxPlainBytes, decompressionConfig.decoderChunkSize, *dst);
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
      if (!ctx.decompressChunk(chunk, lastChunk, maxPlainBytes, decompressionConfig.decoderChunkSize, *dst)) {
        return false;
      }
    }
    return true;
  };
#endif

  // Preserve request trailers (HTTP/1 chunked only). Trailer bytes are stored at the end of the buffer.
  const std::size_t trailersSize = trailerStartPos > 0 ? bodyAndTrailersBuffer.size() - trailerStartPos : 0;
  trailersScratch.assign(bodyAndTrailersBuffer.data() + trailerStartPos, trailersSize);

  bool onlyIdentity = true;

  // Decode in reverse order, algorithm by algorithm, switching src/dst buffers each time.
  const char* first = encodingStr.data();
  const char* last = first + encodingStr.size();
  while (first < last) {
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
      return {.status = http::StatusCodeBadRequest, .message = "Malformed Content-Encoding"};
    }

    // go to next non-OWS/comma char to the left and reject multiple commas in a row
    bool seenComma = false;
    while (nextSep >= first && (http::IsHeaderWhitespace(*nextSep) || *nextSep == ',')) {
      if (*nextSep == ',') {
        if (seenComma) {
          // two commas in a row, malformed
          return {.status = http::StatusCodeBadRequest, .message = "Malformed Content-Encoding"};
        }
        seenComma = true;
      }
      --nextSep;
    }
    last = nextSep + 1;

    // Perform the decoding stage
    bool stageOk = false;
    if (CaseInsensitiveEqual(encoding, http::identity)) {
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
      return {.status = http::StatusCodeUnsupportedMediaType, .message = "Unsupported Content-Encoding"};
    }

    if (!stageOk) {
      return {.status = http::StatusCodeBadRequest, .message = "Decompression failed"};
    }

    if (decompressionConfig.maxExpansionRatio > 0.0) {
      const double ratio = static_cast<double>(dst->size()) / static_cast<double>(originalCompressedSize);
      if (ratio > decompressionConfig.maxExpansionRatio) {
        return {.status = http::StatusCodePayloadTooLarge, .message = "Decompression expansion too large"};
      }
    }

    onlyIdentity = false;

    src = *dst;
    dst = dst == &bodyAndTrailersBuffer ? &tmpBuffer : &bodyAndTrailersBuffer;
  }

  // at this point, src points to the final decompressed data buffer (either tmpBuffer or bodyAndTrailersBuffer)

  if (src.data() == tmpBuffer.data()) {
    // make sure the final result is in bodyAndTrailersBuffer
    tmpBuffer.swap(bodyAndTrailersBuffer);
  } else if (onlyIdentity) {
    // No decoding was actually performed (only identity codings)
    return {};
  }

  RawChars& buf = bodyAndTrailersBuffer;

  const std::size_t decompressedSizeNbChars = static_cast<std::size_t>(nchars(src.size()));
  buf.ensureAvailableCapacity(trailersScratch.size() + decompressedSizeNbChars);

  // Warning: set the new decompressed body AFTER reallocating the buffer above.
  request._body = bodyAndTrailersBuffer;

  if (!trailersScratch.empty()) {
    trailerStartPos = buf.size();
    buf.unchecked_append(trailersScratch);
  }

  std::string_view decompressedSizeStr(buf.end(), decompressedSizeNbChars);
  [[maybe_unused]] const auto [ptr, errc] = std::to_chars(buf.end(), buf.end() + decompressedSizeNbChars, src.size());
  assert(errc == std::errc{} && ptr == buf.end() + decompressedSizeNbChars);
  buf.addSize(decompressedSizeNbChars);

  // Update Content encoding and Content-Length headers, and set special aeronet headers containing orignal values.
  const std::string_view originalContentLenStr = contentLenIt != headersMap.end() ? contentLenIt->second : "";
  headersMap.erase(encodingHeaderIt);
  headersMap.insert_or_assign(http::ContentLength, decompressedSizeStr);
  headersMap.insert_or_assign(http::OriginalEncodingHeaderName, encodingStr);
  if (!originalContentLenStr.empty()) {
    headersMap.insert_or_assign(http::OriginalEncodedLengthHeaderName, originalContentLenStr);
  }

  return {};
}

}  // namespace aeronet::internal
