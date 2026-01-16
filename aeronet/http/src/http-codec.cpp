#include "aeronet/http-codec.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
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
#include "aeronet/nchars.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/stringconv.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-decoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#endif

#ifdef AERONET_ENABLE_ZSTD
#include "aeronet/zstd-decoder.hpp"
#endif

namespace aeronet::internal {

namespace {

// Helper to iterate Content-Encoding header values in reverse order.
class CSVReverseTokensIterator {
 public:
  explicit CSVReverseTokensIterator(std::string_view headerValue)
      : first(headerValue.data()), last(first + headerValue.size()) {}

  // Returns true if there is another value to read.
  [[nodiscard]] bool hasNext() const noexcept { return first < last; }

  // Returns the next value token (trimmed), or empty string_view if malformed.
  std::string_view next() {
    // Header values are already trimmed, so we should not start with an OWS char.
    assert(!http::IsHeaderWhitespace(*(last - 1)));

    // Let's find the next separator to the left, that is the next comma, or OWS, or last.
    const char* nextSep = last - 1;
    while (nextSep >= first && *nextSep != ',' && !http::IsHeaderWhitespace(*nextSep)) {
      --nextSep;
    }

    std::string_view value(nextSep + 1, last);
    if (value.empty()) {
      // empty token forbidden
      return value;
    }

    // go to next non-OWS/comma char to the left and reject multiple commas in a row
    bool seenComma = false;
    while (nextSep >= first && (http::IsHeaderWhitespace(*nextSep) || *nextSep == ',')) {
      if (*nextSep == ',') {
        if (seenComma) {
          // two commas in a row, malformed
          value = {};
          return value;
        }
        seenComma = true;
      }
      --nextSep;
    }
    last = nextSep + 1;

    return value;
  }

 private:
  const char* first;
  const char* last;
};

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
    for (CSVReverseTokensIterator it(value); it.hasNext();) {
      const std::string_view token = it.next();
      assert(!token.empty() && "Malformed Vary header value");
      if (token == "*" || CaseInsensitiveEqual(token, http::AcceptEncoding)) {
        res.valueFirst = kVaryAcceptEncodingNotNeeded;
        return res;
      }
    }

    res.valueFirst = static_cast<std::size_t>(value.data() - base);
    res.valueLast = static_cast<std::size_t>(res.valueFirst + value.size());

    // HttpResponse's public API cannot create multiple headers with the same name,
    // so we can stop here.
    break;
  }

  return res;
}

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

inline RequestDecompressionResult DualBufferDecodeLoop([[maybe_unused]] auto&& runDecoder, double maxExpansionRatio,
                                                       std::string_view& src, HeadersViewMap::iterator encodingHeaderIt,
                                                       std::size_t compressedSize, RawChars& bodyAndTrailersBuffer,
                                                       RawChars& tmpBuffer) {
  RawChars* dst = &tmpBuffer;

  http::StatusCode decompressStatus = http::StatusCodeNotModified;

  // Decode in reverse order, algorithm by algorithm.
  // For the first stage, we read from chunks. For subsequent stages, we read from the previous output buffer.
  for (CSVReverseTokensIterator encIt(encodingHeaderIt->second); encIt.hasNext();) {
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

void ResponseCompressionState::createEncoders([[maybe_unused]] const CompressionConfig& cfg) {
#ifdef AERONET_ENABLE_ZLIB
  gzipEncoder = ZlibEncoder(ZStreamRAII::Variant::gzip, sharedBuffer, cfg);
  deflateEncoder = ZlibEncoder(ZStreamRAII::Variant::deflate, sharedBuffer, cfg);
#endif
#ifdef AERONET_ENABLE_ZSTD
  zstdEncoder = ZstdEncoder(sharedBuffer, cfg);
#endif
#ifdef AERONET_ENABLE_BROTLI
  brotliEncoder = BrotliEncoder(sharedBuffer, cfg);
#endif
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
    return gzipEncoder.encodeFull(data, availableCapacity, buf);
  }
  if (encoding == Encoding::deflate) {
    return deflateEncoder.encodeFull(data, availableCapacity, buf);
  }
#endif
#ifdef AERONET_ENABLE_ZSTD
  if (encoding == Encoding::zstd) {
    return zstdEncoder.encodeFull(data, availableCapacity, buf);
  }
#endif
  throw std::invalid_argument("No encoder for 'none' encoding");
}

std::unique_ptr<EncoderContext> ResponseCompressionState::makeContext([[maybe_unused]] Encoding encoding) {
#ifdef AERONET_ENABLE_BROTLI
  if (encoding == Encoding::br) {
    return brotliEncoder.makeContext();
  }
#endif
#ifdef AERONET_ENABLE_ZLIB
  if (encoding == Encoding::gzip) {
    return gzipEncoder.makeContext();
  }
  if (encoding == Encoding::deflate) {
    return deflateEncoder.makeContext();
  }
#endif
#ifdef AERONET_ENABLE_ZSTD
  if (encoding == Encoding::zstd) {
    return zstdEncoder.makeContext();
  }
#endif
  throw std::invalid_argument("No encoder for 'none' encoding");
}

void HttpCodec::TryCompressResponse(ResponseCompressionState& compressionState,
                                    const CompressionConfig& compressionConfig, std::string_view requestAcceptEncoding,
                                    HttpResponse& resp) {
  const auto bodySz = resp.bodyInMemoryLength();
  if (bodySz < compressionConfig.minBytes) {
    return;
  }
  const auto [encoding, reject] = compressionState.selector.negotiateAcceptEncoding(requestAcceptEncoding);
  // If the client explicitly forbids identity (identity;q=0) and we have no acceptable
  // alternative encodings to offer, emit a 406 per RFC 9110 Section 12.5.3 guidance.
  if (reject) {
    resp.status(http::StatusCodeNotAcceptable).body("No acceptable content-coding available");
    return;
  }
  if (encoding == Encoding::none) {
    return;
  }

  if (!compressionConfig.contentTypeAllowList.empty()) {
    const std::string_view contentType = resp.headerValueOrEmpty(http::ContentType);
    if (!compressionConfig.contentTypeAllowList.containsCI(contentType)) {
      return;
    }
  }

  if (resp.hasHeader(http::ContentEncoding)) {
    return;
  }

  const std::string_view contentEncodingStr = GetEncodingStr(encoding);

  // Sanity check: Content-Type header must be present to consider compression.
  // Normally handled by HttpResponse automatically when user adds a body.
  assert(resp.hasHeader(http::ContentType));

  const bool hasExternalPayload = resp.hasBodyCaptured();
  const std::size_t trailersLen = resp._trailerLen;

  const char* const basePtr = resp._data.data();
  const VaryResult varyResult =
      compressionConfig.addVaryAcceptEncodingHeader
          ? VaryContainsAcceptEncoding(resp, basePtr)
          : VaryResult{.valueFirst = kVaryAcceptEncodingNotNeeded, .valueLast = kVaryAcceptEncodingNotNeeded};
  const bool needVaryAcceptEncoding = !varyResult.notNeeded();
  const bool addVaryHeaderLine = varyResult.absent();
  const std::size_t upperVaryAppendLen =
      (needVaryAcceptEncoding && !addVaryHeaderLine) ? (http::AcceptEncoding.size() + kVaryHeaderValueSep.size()) : 0UL;
  const std::size_t contentEncodingHeaderLineSz =
      HttpResponse::HeaderSize(http::ContentEncoding.size(), contentEncodingStr.size());

  static constexpr std::string_view kVaryHeaderLine =
      JoinStringView_v<http::CRLF, http::Vary, http::HeaderSep, http::AcceptEncoding>;

  const std::size_t varyHeaderLineSz = addVaryHeaderLine ? kVaryHeaderLine.size() : 0UL;

  // Compute offsets for the reserved tail (Content-Type + Content-Length + DoubleCRLF).
  const std::size_t contentTypeLinePos =
      static_cast<std::size_t>(resp.getContentTypeHeaderLinePtr(bodySz) - resp._data.data());
  const std::size_t contentLengthLinePos =
      static_cast<std::size_t>(resp.getContentLengthHeaderLinePtr(bodySz) - resp._data.data());

  const std::size_t oldDataSz = resp._data.size();

  // Reserve once (no realloc after we start reading internal body).
  // We reserve for:
  //   - worst-case tail growth (using current body digit count as upper bound)
  //   - temp compressed output (capped by maxAllowedCompressed + 1)
  //   - final compressed output (capped by maxAllowedCompressed)
  const std::size_t contentTypeLineLen = contentLengthLinePos - contentTypeLinePos;
  const auto upperContentLengthLineLen =
      HttpResponse::HeaderSize(http::ContentLength.size(), static_cast<std::size_t>(nchars(bodySz)));
  const std::size_t upperTailLen = varyHeaderLineSz + contentEncodingHeaderLineSz + contentTypeLineLen +
                                   upperContentLengthLineLen + http::DoubleCRLF.size();

  // We will only commit compression if the configured compression ratio is satisfied.
  const std::size_t maxAllowedCompressed =
      static_cast<std::size_t>(std::ceil(static_cast<double>(bodySz) * compressionConfig.maxCompressRatio));
  assert(maxAllowedCompressed != 0);

  const std::size_t tmpAreaStartPos =
      std::max(oldDataSz + upperVaryAppendLen, contentTypeLinePos + upperTailLen + upperVaryAppendLen);
  const std::size_t upperFinalSize =
      contentTypeLinePos + upperTailLen + maxAllowedCompressed + trailersLen + upperVaryAppendLen;
  const std::size_t upperTempEnd = tmpAreaStartPos + maxAllowedCompressed + trailersLen;
  const std::size_t upperNeededEnd = std::max(upperFinalSize, upperTempEnd);

  assert(oldDataSz < upperNeededEnd);

  resp._data.ensureAvailableCapacity(upperNeededEnd - oldDataSz);

  char* pTmpCompressed = resp._data.data() + tmpAreaStartPos;
  const std::size_t compressedSize =
      compressionState.encodeFull(encoding, resp.bodyInMemory(), maxAllowedCompressed, pTmpCompressed);
  if (compressedSize == 0) {
    // compression failed or did not fit in maxAllowedCompressed
    // abort compression, it's not worth it
    return;
  }

  // If the trailers view are internal resp._data, it may be overwritten when we move back the new body + headers to
  // their final position. So we copy them to a temporary buffer before starting compression.
  const char* pTrailers = nullptr;
  if (trailersLen != 0) {
    pTrailers = resp.trailersFlatView().data();
    if (hasExternalPayload) {
      char* const pTmpTrailers = resp._data.data() + (tmpAreaStartPos + maxAllowedCompressed);
      std::memcpy(pTmpTrailers, pTrailers, trailersLen);
      pTrailers = pTmpTrailers;
    }
  }

  // Apply Vary: Accept-Encoding addition only once we know compression is committed.
  // IMPORTANT: This can shift the whole response buffer, so the temporary compressed bytes must
  // live strictly beyond (oldDataSz + potential insertion length).
  if (needVaryAcceptEncoding) {
    if (addVaryHeaderLine) {
      // Handled later by inserting a new header line next to Content-Type.
    } else {
      // Merge into existing Vary value.
      char* const base = resp._data.data();
      const auto insertPos = varyResult.valueLast;
      assert(insertPos != kNoVaryHeader && insertPos != kVaryAcceptEncodingNotNeeded);

      const bool hasValue = varyResult.valueFirst != varyResult.valueLast;

      const std::size_t extraLen = (hasValue ? kVaryHeaderValueSep.size() : 0UL) + http::AcceptEncoding.size();

      // Insert at insertPos (end of Vary value).
      char* const moveSrc = base + insertPos;
      const std::size_t tailLen = resp._data.size() - insertPos;
      std::memmove(moveSrc + extraLen, moveSrc, tailLen);
      char* out = moveSrc;
      if (hasValue) {
        std::memcpy(out, kVaryHeaderValueSep.data(), kVaryHeaderValueSep.size());
        out += kVaryHeaderValueSep.size();
      }
      std::memcpy(out, http::AcceptEncoding.data(), http::AcceptEncoding.size());
      resp._data.addSize(extraLen);
      resp.adjustBodyStart(static_cast<int64_t>(extraLen));
    }
  }

  // Recompute tail offsets after the optional Vary merge above (it can shift Content-Type/Length positions).
  const std::size_t contentTypeLinePos2 =
      static_cast<std::size_t>(resp.getContentTypeHeaderLinePtr(bodySz) - resp._data.data());
  const std::size_t contentLengthLinePos2 =
      static_cast<std::size_t>(resp.getContentLengthHeaderLinePtr(bodySz) - resp._data.data());

  const std::size_t contentTypeLineLen2 = contentLengthLinePos2 - contentTypeLinePos2;

  const uint32_t nbCharsCompressedSize = static_cast<uint32_t>(nchars(compressedSize));
  const std::size_t newContentLengthLineLen =
      HttpResponse::HeaderSize(http::ContentLength.size(), nbCharsCompressedSize);
  const std::size_t newTailLen = varyHeaderLineSz + contentEncodingHeaderLineSz + contentTypeLineLen2 +
                                 newContentLengthLineLen + http::DoubleCRLF.size();

  const std::size_t newContentTypeLinePos = contentTypeLinePos2 + varyHeaderLineSz + contentEncodingHeaderLineSz;
  const std::size_t newContentLengthLinePos = newContentTypeLinePos + contentTypeLineLen2;
  const std::size_t newBodyStartPos = contentTypeLinePos2 + newTailLen;

  // Move existing Content-Type line away from the insertion zone
  std::memmove(resp._data.data() + newContentTypeLinePos, resp._data.data() + contentTypeLinePos2, contentTypeLineLen2);

  // Write the newly inserted headers
  char* out = resp._data.data() + contentTypeLinePos2;
  if (addVaryHeaderLine) {
    std::memcpy(out, kVaryHeaderLine.data(), kVaryHeaderLine.size());
    out += kVaryHeaderLine.size();
  }
  out = WriteCRLFHeader(out, http::ContentEncoding, contentEncodingStr);

  // Write updated Content-Length and double CRLF after the moved Content-Type line
  out = resp._data.data() + newContentLengthLinePos;

  static constexpr std::string_view kContentLengthPrefix =
      JoinStringView_v<http::CRLF, http::ContentLength, http::HeaderSep>;

  std::memcpy(out, kContentLengthPrefix.data(), kContentLengthPrefix.size());
  out += kContentLengthPrefix.size();
  [[maybe_unused]] const auto tcRes = std::to_chars(out, out + nbCharsCompressedSize, compressedSize);
  assert(tcRes.ec == std::errc{} && tcRes.ptr == out + nbCharsCompressedSize);
  out += nbCharsCompressedSize;

  std::memcpy(out, http::DoubleCRLF.data(), http::DoubleCRLF.size());
  out += http::DoubleCRLF.size();
  assert(std::cmp_equal(out - resp._data.data(), newBodyStartPos));

  // Move compressed body to its final position.
  char* newBodyStartPtr = resp._data.data() + newBodyStartPos;
  std::memmove(newBodyStartPtr, pTmpCompressed, compressedSize);

  // Move/copy trailers to final position without heap temps.
  if (trailersLen != 0) {
    char* newTrailerStartPtr = newBodyStartPtr + compressedSize;
    if (hasExternalPayload) {
      std::memcpy(newTrailerStartPtr, pTrailers, trailersLen);
    } else {
      std::memmove(newTrailerStartPtr, resp._data.data() + oldDataSz - trailersLen, trailersLen);
    }
  }

  resp.setBodyStartPos(newBodyStartPos);
  resp._data.setSize(newBodyStartPos + compressedSize + trailersLen);
  resp._payloadVariant = {};
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
  CSVReverseTokensIterator encIt(encodingHeaderIt->second);
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
