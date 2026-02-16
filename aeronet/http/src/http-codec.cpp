#include "aeronet/http-codec.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/compression-config.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/stringconv.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#endif

namespace aeronet::internal {

namespace {

constexpr std::size_t kNoVaryHeader = 0;
constexpr std::size_t kVaryAcceptEncodingNotNeeded = 1;

constexpr std::string_view kVaryHeaderValueSep = ", ";

struct VaryResult {
  [[nodiscard]] bool absent() const noexcept { return valueFirst == kNoVaryHeader; }
  [[nodiscard]] bool notNeeded() const noexcept { return valueFirst == kVaryAcceptEncodingNotNeeded; }

  std::size_t valueFirst;
  std::size_t valueLast;
};

// Returns:
// - 0: no Vary header exists
// - npos: Vary already covers Accept-Encoding (either explicitly or via '*')
// - otherwise: insertion offset (from resp._data.data()) at the end of the Vary value,
//   suitable for inserting either ", Accept-Encoding" or "Accept-Encoding".
[[nodiscard]] VaryResult VaryContainsAcceptEncoding(const HttpResponse& resp, const char* base) {
  VaryResult res{.valueFirst = kNoVaryHeader, .valueLast = kNoVaryHeader};

  for (const auto& hdr : resp.headers()) {
    if (!CaseInsensitiveEqual(hdr.name, http::Vary)) {
      continue;
    }

    // Response headers are guaranteed to be trimmed on the extreme sides.
    // We can therefore use the reverse CSV iterator (order doesn't matter).
    const std::string_view value = hdr.value;
    for (http::HeaderValueReverseTokensIterator<','> it(value); it.hasNext();) {
      const std::string_view token = it.next();
      if (token == "*" || CaseInsensitiveEqual(token, http::AcceptEncoding)) {
        res.valueFirst = kVaryAcceptEncodingNotNeeded;
        return res;
      }
    }

    res.valueFirst = static_cast<std::size_t>(value.data() - base);
    res.valueLast = static_cast<std::size_t>(res.valueFirst + value.size());

    // Heuristic - we suppose that there will not be multiple Vary headers in the response.
    break;
  }

  return res;
}

inline std::string_view FinalizeDecompressedBody(HeadersViewMap& headersMap, HeadersViewMap::iterator encodingHeaderIt,
                                                 std::string_view src, std::size_t additionalCapacity, RawChars& buf) {
  const std::size_t decompressedSizeNbChars = nchars(src.size());
  buf.ensureAvailableCapacity(decompressedSizeNbChars + additionalCapacity);

  // Set the new decompressed body AFTER reallocating the buffer above.
  std::string_view body = buf;

  std::string_view decompressedSizeStr(buf.end(), decompressedSizeNbChars);
  [[maybe_unused]] const auto [ptr, errc] = std::to_chars(buf.end(), buf.end() + decompressedSizeNbChars, src.size());
  assert(errc == std::errc{} && ptr == buf.end() + decompressedSizeNbChars);
  buf.addSize(decompressedSizeNbChars);

  // Update Content encoding and Content-Length headers, and set special aeronet headers containing original values.
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

inline RequestDecompressionResult DualBufferDecodeLoop([[maybe_unused]] RequestDecompressionState& decompressionState,
                                                       [[maybe_unused]] auto&& runDecoder, double maxExpansionRatio,
                                                       std::string_view& src, HeadersViewMap::iterator encodingHeaderIt,
                                                       std::size_t compressedSize, RawChars& bodyAndTrailersBuffer,
                                                       RawChars& tmpBuffer) {
  RawChars* dst = &tmpBuffer;

  http::StatusCode decompressStatus = http::StatusCodeNotModified;

  // Decode in reverse order, algorithm by algorithm.
  // For the first stage, we read from chunks. For subsequent stages, we read from the previous output buffer.
  for (http::HeaderValueReverseTokensIterator<','> encIt(encodingHeaderIt->second); encIt.hasNext();) {
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
      ZlibDecoder& decoder = decompressionState.zlibDecoder;
      decoder.setVariant(ZStreamRAII::Variant::gzip);
      stageOk = runDecoder(decoder, *dst);
    } else if (CaseInsensitiveEqual(encoding, http::deflate)) {
      ZlibDecoder& decoder = decompressionState.zlibDecoder;
      decoder.setVariant(ZStreamRAII::Variant::deflate);
      stageOk = runDecoder(decoder, *dst);
#endif
#ifdef AERONET_ENABLE_ZSTD
    } else if (CaseInsensitiveEqual(encoding, http::zstd)) {
      stageOk = runDecoder(decompressionState.zstdDecoder, *dst);
#endif
#ifdef AERONET_ENABLE_BROTLI
    } else if (CaseInsensitiveEqual(encoding, http::br)) {
      stageOk = runDecoder(decompressionState.brotliDecoder, *dst);
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

ResponseCompressionState::ResponseCompressionState(const CompressionConfig& cfg)
    : selector(cfg),
      pCompressionConfig(&cfg)
#ifdef AERONET_ENABLE_BROTLI
      ,
      brotliEncoder(pCompressionConfig->brotli)
#endif
#ifdef AERONET_ENABLE_ZLIB
      ,
      zlibEncoder(pCompressionConfig->zlib.level)
#endif
#ifdef AERONET_ENABLE_ZSTD
      ,
      zstdEncoder(pCompressionConfig->zstd)
#endif
{
}

std::size_t ResponseCompressionState::encodeFull([[maybe_unused]] Encoding encoding,
                                                 [[maybe_unused]] std::string_view data,
                                                 [[maybe_unused]] std::size_t availableCapacity,
                                                 [[maybe_unused]] char* buf) {
#ifdef AERONET_ENABLE_BROTLI
  if (encoding == Encoding::br) {
    return brotliEncoder.encodeFull(data, availableCapacity, buf);
  }
#endif
#ifdef AERONET_ENABLE_ZLIB
  if (encoding == Encoding::gzip) {
    return zlibEncoder.encodeFull(ZStreamRAII::Variant::gzip, data, availableCapacity, buf);
  }
  if (encoding == Encoding::deflate) {
    return zlibEncoder.encodeFull(ZStreamRAII::Variant::deflate, data, availableCapacity, buf);
  }
#endif
#ifdef AERONET_ENABLE_ZSTD
  if (encoding == Encoding::zstd) {
    return zstdEncoder.encodeFull(data, availableCapacity, buf);
  }
#endif
  throw std::invalid_argument("Unsupported encoding for encodeFull");
}

EncoderContext* ResponseCompressionState::makeContext([[maybe_unused]] Encoding encoding) {
#ifdef AERONET_ENABLE_BROTLI
  if (encoding == Encoding::br) {
    return brotliEncoder.makeContext();
  }
#endif
#ifdef AERONET_ENABLE_ZLIB
  if (encoding == Encoding::gzip) {
    return zlibEncoder.makeContext(ZStreamRAII::Variant::gzip);
  }
  if (encoding == Encoding::deflate) {
    return zlibEncoder.makeContext(ZStreamRAII::Variant::deflate);
  }
#endif
#ifdef AERONET_ENABLE_ZSTD
  if (encoding == Encoding::zstd) {
    return zstdEncoder.makeContext();
  }
#endif
  throw std::invalid_argument("Unsupported encoding for makeContext");
}

EncoderContext* ResponseCompressionState::context([[maybe_unused]] Encoding encoding) {
#ifdef AERONET_ENABLE_BROTLI
  if (encoding == Encoding::br) {
    return brotliEncoder.context();
  }
#endif
#ifdef AERONET_ENABLE_ZLIB
  if (encoding == Encoding::deflate || encoding == Encoding::gzip) {
    return zlibEncoder.context();
  }
#endif
#ifdef AERONET_ENABLE_ZSTD
  if (encoding == Encoding::zstd) {
    return zstdEncoder.context();
  }
#endif
  throw std::invalid_argument("Unsupported encoding for makeContext");
}

namespace {

constexpr std::string_view kCRLFVaryAcceptEncodingLine =
    JoinStringView_v<http::CRLF, http::Vary, http::HeaderSep, http::AcceptEncoding>;

constexpr std::string_view kCRLFContentLengthHeaderSep =
    JoinStringView_v<http::CRLF, http::ContentLength, http::HeaderSep>;

std::size_t ComputeAdditionalVaryLength(bool needVaryAcceptEncoding, bool addVaryHeaderLine, bool hasVaryHeader) {
  if (!needVaryAcceptEncoding) {
    return 0UL;
  }
  if (addVaryHeaderLine) {
    return kCRLFVaryAcceptEncodingLine.size();
  }
  if (!hasVaryHeader) {
    return http::AcceptEncoding.size();
  }
  return kVaryHeaderValueSep.size() + http::AcceptEncoding.size();
}

}  // namespace

void HttpCodec::TryCompressResponse(ResponseCompressionState& compressionState, Encoding encoding, HttpResponse& resp) {
  const auto bodySz = resp.bodyInMemoryLength();
  const auto& compressionConfig = *compressionState.pCompressionConfig;

  if (bodySz < compressionConfig.minBytes) {
    return;
  }
  if (resp.hasContentEncoding()) {
    return;
  }
  if (!compressionConfig.contentTypeAllowList.empty()) {
    const std::string_view contentType = resp.headerValueOrEmpty(http::ContentType);
    if (!compressionConfig.contentTypeAllowList.containsCI(contentType)) {
      return;
    }
  }

  // Sanity check: Content-Type header must be present to consider compression.
  // Normally handled by HttpResponse automatically when user adds a body.
  assert(resp.hasHeader(http::ContentType));

  // At this step, we will try the compression.
  // We have two cases - either the body is in the main buffer, or it is captured.

  // We will only commit compression if the configured compression ratio is satisfied.
  const std::size_t maxCompressedBytes = compressionConfig.maxCompressedBytes(bodySz);
  const std::string_view contentEncodingStr = GetEncodingStr(encoding);

  const bool hasBodyCaptured = resp.hasBodyCaptured();
  const auto trailersSz = resp.trailersSize();

  const char* pData = resp._data.data();
  const VaryResult varyResult =
      compressionConfig.addVaryAcceptEncodingHeader
          ? VaryContainsAcceptEncoding(resp, pData)
          : VaryResult{.valueFirst = kVaryAcceptEncodingNotNeeded, .valueLast = kVaryAcceptEncodingNotNeeded};
  const bool hasVaryHeader = varyResult.valueFirst != varyResult.valueLast;
  const bool needVaryAcceptEncoding = !varyResult.notNeeded();
  const bool addVaryHeaderLine = varyResult.absent();
  const std::size_t additionalVaryLen =
      ComputeAdditionalVaryLength(needVaryAcceptEncoding, addVaryHeaderLine, hasVaryHeader);
  const std::size_t contentEncodingHeaderLineSz =
      HttpResponse::HeaderSize(http::ContentEncoding.size(), contentEncodingStr.size());

  // Compute offsets for the reserved tail (Content-Type + Content-Length + DoubleCRLF).
  const auto nCharsBodySz = nchars(bodySz);
  const auto nCharsMaxCompressedSize = nchars(maxCompressedBytes);
  std::size_t contentTypeLinePos = static_cast<std::size_t>(resp.getContentTypeHeaderLinePtr() - pData);
  std::size_t contentLengthLinePos = static_cast<std::size_t>(resp.getContentLengthHeaderLinePtr() - pData);

  const std::size_t oldDataSz = resp._data.size();

  // Reserve once (no realloc after we start reading internal body).
  // We reserve for:
  //   - worst-case tail growth (using current body digit count as upper bound)
  //   - temp compressed output (capped by maxCompressedBytes + 1)
  //   - final compressed output (capped by maxCompressedBytes)
  const std::size_t contentTypeLineLen = contentLengthLinePos - contentTypeLinePos;
  const auto contentLengthLineLen = HttpResponse::HeaderSize(http::ContentLength.size(), nCharsBodySz);
  const auto upperContentLengthLineLen = HttpResponse::HeaderSize(http::ContentLength.size(), nCharsMaxCompressedSize);

  static_assert(
      HttpResponse::HeaderSize(http::ContentEncoding.size(), 1U) >= std::numeric_limits<std::size_t>::digits10 + 1,
      "headersShift cannot be negative for below logic");

  const std::size_t headersShift =
      additionalVaryLen + contentEncodingHeaderLineSz + upperContentLengthLineLen - contentLengthLineLen;
  const std::size_t capturedTrailerGrowth = hasBodyCaptured ? trailersSz : 0UL;
  const auto neededCapacity = oldDataSz + maxCompressedBytes + headersShift + capturedTrailerGrowth;

  // unique reallocation
  resp._data.reserve(neededCapacity);

  // Note: we are before the finalization of the HttpResponse here, so there is no Transfer-Encoding: chunked case to
  // worry about (with tail \r\n0\r\n[trailers]\r\n). The trailers are directly after the body if they exist (not even a
  // CRLF splits the body and the first trailer line).
  char* pCompBody = resp._data.data() + oldDataSz + headersShift;
  const auto availableCompCapa = resp._data.capacity() - oldDataSz - headersShift;
  const std::size_t compressedSize =
      compressionState.encodeFull(encoding, resp.bodyInMemory(), availableCompCapa, pCompBody);

  if (compressedSize == 0) {
    // compression failed or did not fit in maxCompressedBytes - abort compression and leave the response unmodified.
    // TODO: increase telemetry counter?
    return;
  }

  // Compression succeeded at this point, we can start to move parts to their final position.
  // We will move the HttpResponse parts from backwards to avoid overwriting data, in this order:
  // 1) trailers
  // 2) compressed body (only if inlined, for captured bodies the compressed body is already in its final position)
  // 3) Double CRLF (before the compressed body)
  // 4) new Content-Length header line (with padding if needed)
  // 5) Content-Type header line (moved)
  // 6) new Content-Encoding header line (added)
  // 7) new Vary header line or value update (added or updated if needed)
  //
  //  For inline bodies:
  //   [headers][content-type][content-length][CRLF][uncompressed body][trailers][<headersShift>][compressed body]
  //  For captured bodies:
  //   [headers][content-type][content-length][CRLF][<headersShift>][compressed body]
  //   [uncompressed body][trailers]

  // Copies trailers to their final position.
  // For captured bodies (and trailers), we can use memcpy because the buffers do not overlap.
  if (trailersSz != 0) {
    const char* pTrailers = resp.trailersFlatView().data();
    char* pDest;
    if (hasBodyCaptured) {
      pDest = resp._data.data() + oldDataSz + headersShift + compressedSize;
      std::memcpy(pDest, pTrailers, trailersSz);
    } else {
      pDest = resp._data.data() + (pTrailers - resp._data.data()) + headersShift + compressedSize - bodySz;
      std::memmove(pDest, pTrailers, trailersSz);
    }
  }

  // Buffer layout after trailers move to their final position:
  //  For inline bodies:
  //   [headers][content-type][content-length][CRLF][???][trailers][???][compressed body]
  //  For captured bodies:
  //   [headers][content-type][content-length][CRLF][<headersShift>][compressed body][trailers]
  //   [uncompressed body]

  const auto newBodyStartPos = resp.bodyStartPos() + headersShift;
  if (!hasBodyCaptured) {
    // Move body to its final position (after the reserved tail and after the potential headers inserted for Vary and
    // Content-Encoding).
    std::memcpy(resp._data.data() + newBodyStartPos, pCompBody, compressedSize);
    // Update body start position to the new location.
    resp.setBodyStartPos(newBodyStartPos);
  }

  // At this point, compressed body and trailers (if exist) are in their final position, but headers are not updated yet
  // (Content-Length, new headers for Content-Encoding and potentially Vary) and not moved yet to their final position.

  // Buffer layout after compressed body move to final position (only for inline bodies, for captured bodies the
  // compressed body is already in its final position):
  //  [headers][content-type][content-length][CRLF][<headersShift>][compressed body][trailers]

  // Write DoubleCRLF before the compressed body.
  char* out = resp._data.data() + newBodyStartPos - http::DoubleCRLF.size();
  Copy(http::DoubleCRLF, out);

  // Write new Content-Length, padded with spaces if the number of chars of the actual compressed size is smaller than
  // the number of chars of the declared max compressed size (worst case).
  [[maybe_unused]] const auto tcRes = std::to_chars(out - nCharsMaxCompressedSize, out, compressedSize);
  assert(tcRes.ec == std::errc{});
  std::fill(tcRes.ptr, out, ' ');  // pad with spaces if needed
  out -= nCharsMaxCompressedSize;

  // Write '\r\nContent-Length: ' just before the new Content-Length value.
  Copy(kCRLFContentLengthHeaderSep, out - kCRLFContentLengthHeaderSep.size());
  out -= kCRLFContentLengthHeaderSep.size();

  // Write '\r\nContent-Type: XXXX' just before the Content-Length line.
  std::memmove(out - contentTypeLineLen, resp._data.data() + contentTypeLinePos, contentTypeLineLen);
  out -= contentTypeLineLen;

  // Write new '\r\nContent-Encoding: XXXX' header.
  WriteCRLFHeader(out - contentEncodingHeaderLineSz, http::ContentEncoding, contentEncodingStr);
  out -= contentEncodingHeaderLineSz;

  // Write '\r\nVary: Accept-Encoding' if needed.
  if (addVaryHeaderLine) {
    Copy(kCRLFVaryAcceptEncodingLine, out - kCRLFVaryAcceptEncodingLine.size());
  } else if (needVaryAcceptEncoding) {
    // We are in the case of an existing Vary header without Accept-Encoding, we will append ", Accept-Encoding" to it.
    // The insertion point is guaranteed to be before the Content-Type line because of the heuristic in
    // VaryContainsAcceptEncoding.
    out = resp._data.data() + varyResult.valueLast;
    const std::size_t tailLen = contentTypeLinePos - varyResult.valueLast;
    std::memmove(out + additionalVaryLen, out, tailLen);
    if (varyResult.valueLast != varyResult.valueFirst) {
      out = Append(kVaryHeaderValueSep, out);
    }
    Copy(http::AcceptEncoding, out);
  }

  // Finalize response metadata to reflect compression
  resp.setBodyStartPos(newBodyStartPos);
  resp._data.setSize(newBodyStartPos + compressedSize + trailersSz);
  resp._payloadVariant = {};
}

RequestDecompressionResult HttpCodec::MaybeDecompressRequestBody(RequestDecompressionState& decompressionState,
                                                                 const DecompressionConfig& decompressionConfig,
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
      decompressionState,
      [useStreamingDecode =
           UseStreamingDecompression(headersMap, decompressionConfig.streamingDecompressionThresholdBytes),
       &src, &decompressionConfig](auto& decoder, RawChars& dst) {
        dst.clear();
        if (!useStreamingDecode) {
          return decoder.decompressFull(src, decompressionConfig.maxDecompressedBytes,
                                        decompressionConfig.decoderChunkSize, dst);
        }
        auto* ctx = decoder.makeContext();
        // The decompress body function cannot be called without body
        assert(!src.empty());
        for (std::size_t processed = 0; processed < src.size();) {
          const std::size_t remaining = src.size() - processed;
          const std::size_t chunkLen = std::min(decompressionConfig.decoderChunkSize, remaining);
          const std::string_view chunk(src.data() + processed, chunkLen);
          processed += chunkLen;
          const bool lastChunk = processed == src.size();
          if (!ctx->decompressChunk(chunk, lastChunk, decompressionConfig.maxDecompressedBytes,
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
  request._body = FinalizeDecompressedBody(headersMap, encodingHeaderIt, src, 0UL, bodyAndTrailersBuffer);

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

  bool foundNonIdentity = false;

  // Parse the encoding string to check for non-identity encodings
  http::HeaderValueReverseTokensIterator<','> encIt(encodingStr);
  assert(encIt.hasNext());
  do {
    std::string_view encoding = encIt.next();
    if (encoding.empty()) {
      return http::StatusCodeBadRequest;
    }
    if (!foundNonIdentity && !CaseInsensitiveEqual(encoding, http::identity)) {
      foundNonIdentity = true;
    }
  } while (encIt.hasNext());

  if (foundNonIdentity) {
    return http::StatusCodeOK;
  }
  // Only identity
  return http::StatusCodeNotModified;
}

RequestDecompressionResult HttpCodec::DecompressChunkedBody(RequestDecompressionState& decompressionState,
                                                            const DecompressionConfig& decompressionConfig,
                                                            HttpRequest& request,
                                                            std::span<const std::string_view> compressedChunks,
                                                            std::size_t compressedSize, RawChars& bodyAndTrailersBuffer,
                                                            RawChars& tmpBuffer) {
  auto& headersMap = request._headers;
  const auto encodingHeaderIt = headersMap.find(http::ContentEncoding);
  assert(encodingHeaderIt != headersMap.end());  // this shouldn't happen, WillDecompress was checked before
  assert(decompressionConfig.enable);

  std::string_view src;

  const auto additionalCapacity = bodyAndTrailersBuffer.size();

  RequestDecompressionResult res = DualBufferDecodeLoop(
      decompressionState,
      [&src, compressedChunks, maxPlainBytes = decompressionConfig.maxDecompressedBytes,
       decoderChunkSize = decompressionConfig.decoderChunkSize,
       firstStage = true](auto& decoder, RawChars& dst) mutable {
        dst.clear();

        if (firstStage) {
          firstStage = false;
          auto* ctx = decoder.makeContext();
          for (std::size_t idx = 0; idx < compressedChunks.size(); ++idx) {
            const bool lastChunk = (idx == compressedChunks.size() - 1);
            if (!ctx->decompressChunk(compressedChunks[idx], lastChunk, maxPlainBytes, decoderChunkSize, dst)) {
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

  request._body =
      FinalizeDecompressedBody(headersMap, encodingHeaderIt, src, additionalCapacity, bodyAndTrailersBuffer);

  return {};
}

}  // namespace aeronet::internal
