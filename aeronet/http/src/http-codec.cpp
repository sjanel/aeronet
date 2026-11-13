#include "aeronet/http-codec.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#ifndef NDEBUG
#include <system_error>
#endif

#include "aeronet/compression-config.hpp"
#include "aeronet/decimal-writer.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/embedded-payload.hpp"
#include "aeronet/encoder-result.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-codec-result.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-value-reverse-tokens-iterator.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/sv-to-sv-map.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include <brotli/encode.h>
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-variant.hpp"
#endif

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>

#include "aeronet/zstd-decoder.hpp"
#endif

namespace aeronet {

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

// The headers of a response that its compression updates.
struct CompressionHeaders {
  // valueFirst is:
  // - kNoVaryHeader: no Vary header exists
  // - kVaryAcceptEncodingNotNeeded: Vary already covers Accept-Encoding (either explicitly or via '*'), or is not added
  // - otherwise: [valueFirst, valueLast) is the Vary value (offsets from the start of the message), Accept-Encoding
  //   being inserted at its end.
  VaryResult vary;
  // Offset of the value of a strong ETag, which the compression makes weak (RFC 9110 §8.8.3.3), or 0.
  std::size_t strongETagPos;
};

// Scans the headers once for the first Vary and ETag headers (a response is supposed to have one of each at most).
[[nodiscard]] CompressionHeaders ScanCompressionHeaders(const HttpMessage& msg, const char* pData, bool addVary) {
  CompressionHeaders res{.vary = {.valueFirst = kNoVaryHeader, .valueLast = kNoVaryHeader}, .strongETagPos = 0};
  if (!addVary) {
    res.vary.valueFirst = res.vary.valueLast = kVaryAcceptEncodingNotNeeded;
  }
  bool varySeen = !addVary;
  bool etagSeen = false;

  for (const auto& hdr : msg.headers()) {
    if (hdr.name == http::ETag) {
      if (!etagSeen) {
        etagSeen = true;
        // A strong entity-tag is a quoted string, a weak one starts with W/.
        if (hdr.value.starts_with('"')) {
          res.strongETagPos = static_cast<std::size_t>(hdr.value.data() - pData);
        }
      }
      continue;
    }
    if (varySeen || hdr.name != http::Vary) {
      continue;
    }
    varySeen = true;

    // Response headers are guaranteed to be trimmed on the extreme sides.
    // We can therefore use the reverse CSV iterator (order doesn't matter).
    const std::string_view value = hdr.value;
    bool covered = false;
    for (http::HeaderValueReverseTokensIterator it(value); it.hasNext();) {
      const std::string_view token = it.next();
      if (token == "*" || CaseInsensitiveEqual(token, http::AcceptEncoding)) {
        covered = true;
        break;
      }
    }
    if (covered) {
      res.vary.valueFirst = res.vary.valueLast = kVaryAcceptEncodingNotNeeded;
    } else {
      res.vary.valueFirst = static_cast<std::size_t>(value.data() - pData);
      res.vary.valueLast = static_cast<std::size_t>(res.vary.valueFirst + value.size());
    }
  }

  return res;
}

// A string to insert at an offset of a message.
struct HeadInsertion {
  std::size_t pos;
  std::string_view str;
};

// Inserts each string at its offset in [pData, pData + endPos), the bytes after it moving forward by the size of the
// strings inserted before them. The insertions are sorted by decreasing offset, and the buffer has room for them all.
void InsertInHead(char* pData, std::size_t endPos, std::span<const HeadInsertion> insertions) {
  std::size_t shift = 0;
  for (const HeadInsertion& insertion : insertions) {
    shift += insertion.str.size();
  }
  for (const auto& [pos, str] : insertions) {
    assert(pos <= endPos);
    std::memmove(pData + pos + shift, pData + pos, endPos - pos);
    shift -= str.size();
    Copy(str, pData + pos + shift);
    endPos = pos;
  }
}

inline std::string_view FinalizeDecompressedBody(SvToSvMap& headersMap, SvToSvMap::iterator encodingHeaderIt,
                                                 std::string_view src, std::size_t additionalCapacity, RawChars& buf) {
  const auto decompressedSizeNbDigits = ndigits(src.size());

  buf.ensureAvailableCapacity(additionalCapacity + decompressedSizeNbDigits);

  // Set the new decompressed body AFTER reallocating the buffer above.
  std::string_view body = buf;

  std::string_view decompressedSizeStr(buf.end(), decompressedSizeNbDigits);
  [[maybe_unused]] const auto ptr = WriteUInt(buf.end(), src.size(), decompressedSizeNbDigits);
  assert(ptr == buf.end() + decompressedSizeNbDigits);
  buf.addSize(decompressedSizeNbDigits);

  // Update Content encoding and Content-Length headers, and set special aeronet headers containing original values.
  const std::string_view encodingStr = encodingHeaderIt->second;
  const auto contentLenIt = headersMap.find(http::ContentLength);
  // This is safe because the string_view is copied - the map is a SvToSvMap, so even if erase / insert invalidates
  // iterators, we have already stored the original value in originalContentLenStr.
  const std::string_view originalContentLenStr = contentLenIt != headersMap.end() ? contentLenIt->second : "";
  headersMap.erase(encodingHeaderIt);
  headersMap.insert_or_assign(http::ContentLength, decompressedSizeStr);
  headersMap.insert_or_assign(http::OriginalEncodingHeaderName, encodingStr);
  if (!originalContentLenStr.empty()) {
    headersMap.insert_or_assign(http::OriginalEncodedLengthHeaderName, originalContentLenStr);
  }

  return body;
}

// Each coding of a Content-Encoding list is a decoding pass over the whole body, and real messages carry one. A longer
// list is refused before decoding anything: curl bounds the codings of a response to 5 too (CVE-2023-23916). Counted
// from the separators: identity counts, as for curl, and so do empty elements, ignored otherwise (RFC 9110 §5.6.1.2:
// "a reasonable number" of them).
constexpr std::size_t kMaxContentCodings = 5;

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
constexpr RequestDecompressionResult kDecompressedSizeTooLarge{
    .status = http::StatusCodePayloadTooLarge,
    .message = "Decompressed size too large",
};

constexpr RequestDecompressionResult kExpansionTooLarge{
    .status = http::StatusCodePayloadTooLarge,
    .message = "Decompression expansion too large",
};

// The number of bytes that each decoding stage may produce, and the error reported beyond.
struct DecodeLimit {
  std::size_t maxPlainBytes;
  const RequestDecompressionResult* pExceeded;
};

// maxDecompressedBytes, or less when maxExpansionRatio times the compressed size of the body is smaller. The decoders
// stop as soon as they exceed it, so that a compression bomb is refused before it is inflated.
DecodeLimit ComputeDecodeLimit(const DecompressionConfig& cfg, std::size_t compressedSize) noexcept {
  const double ratioBound = cfg.maxExpansionRatio * static_cast<double>(compressedSize);
  // Compared in double: the product may not be representable as a std::size_t.
  if (ratioBound < static_cast<double>(cfg.maxDecompressedBytes)) {
    return {.maxPlainBytes = static_cast<std::size_t>(ratioBound), .pExceeded = &kExpansionTooLarge};
  }
  return {.maxPlainBytes = cfg.maxDecompressedBytes, .pExceeded = &kDecompressedSizeTooLarge};
}
#endif

inline RequestDecompressionResult DualBufferDecodeLoop([[maybe_unused]] DecompressionState& decompressionState,
                                                       [[maybe_unused]] auto&& runDecoder,
                                                       [[maybe_unused]] const DecompressionConfig& decompressionConfig,
                                                       std::string_view& src, std::string_view contentEncoding,
                                                       [[maybe_unused]] std::size_t compressedSize,
                                                       RawChars& bodyAndTrailersBuffer, RawChars& tmpBuffer) {
  // Codings are compared exactly: the parsers lower-case received Content-Encoding values.
  assert(!LowerAsciiKey::ContainsUpperAscii(contentEncoding));
  if (std::cmp_greater_equal(std::ranges::count(contentEncoding, ','), kMaxContentCodings)) [[unlikely]] {
    return {.status = http::StatusCodeUnsupportedMediaType, .message = "Too many Content-Encoding codings"};
  }
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  // The ratio is the one of the whole body: every stage has the same limit.
  const DecodeLimit limit = ComputeDecodeLimit(decompressionConfig, compressedSize);
  RawChars* dst = &tmpBuffer;
  bool stageOk{};
#endif

  http::StatusCode decompressStatus = http::StatusCodeNotModified;

  // Decode in reverse order, algorithm by algorithm.
  // For the first stage, we read from chunks. For subsequent stages, we read from the previous output buffer.
  for (http::HeaderValueReverseTokensIterator encIt(contentEncoding); encIt.hasNext();) {
    std::string_view encoding = encIt.next();
    // A recipient MUST ignore the empty elements of a list (RFC 9110 §5.6.1.2).
    if (encoding.empty() || encoding == http::identity) {
      continue;
#ifdef AERONET_ENABLE_ZLIB
      // NOLINTNEXTLINE(readability-else-after-return)
    } else if (IsGzipCoding(encoding)) {
      ZlibDecoder& decoder = decompressionState.zlibDecoder;
      decoder.setVariant(ZlibVariant::gzip);
      stageOk = runDecoder(decoder, *dst, limit.maxPlainBytes);
    } else if (encoding == http::deflate) {
      ZlibDecoder& decoder = decompressionState.zlibDecoder;
      decoder.setVariant(ZlibVariant::deflate);
      stageOk = runDecoder(decoder, *dst, limit.maxPlainBytes);
#endif
#ifdef AERONET_ENABLE_ZSTD
    } else if (encoding == http::zstd) {
      stageOk = runDecoder(decompressionState.zstdDecoder, *dst, limit.maxPlainBytes);
      // decompressFull() rejects a frame declaring a too large size without decoding it.
      if (!stageOk && ZstdDecoder::DeclaresMoreThan(src, limit.maxPlainBytes)) {
        return *limit.pExceeded;
      }
#endif
#ifdef AERONET_ENABLE_BROTLI
    } else if (encoding == http::br) {
      stageOk = runDecoder(decompressionState.brotliDecoder, *dst, limit.maxPlainBytes);
#endif
    } else {
      return {.status = http::StatusCodeUnsupportedMediaType, .message = "Unsupported Content-Encoding"};
    }
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    if (!stageOk) {
      // A decoder exceeding its limit fails with more bytes decoded.
      if (dst->size() > limit.maxPlainBytes) {
        return *limit.pExceeded;
      }
      return {.status = http::StatusCodeBadRequest, .message = "Decompression failed"};
    }
    assert(dst->size() <= limit.maxPlainBytes);

    decompressStatus = http::StatusCodeOK;

    src = *dst;
    dst = dst == &bodyAndTrailersBuffer ? &tmpBuffer : &bodyAndTrailersBuffer;
#endif
  }

  if (src.data() == tmpBuffer.data()) {
    // make sure the final result is in bodyAndTrailersBuffer
    tmpBuffer.swap(bodyAndTrailersBuffer);
  }

  return {.status = decompressStatus, .message = nullptr};
}

inline bool UseStreamingDecompression(const SvToSvMap& headersMap, std::size_t streamingDecompressionThresholdBytes) {
  const auto contentLenIt = headersMap.find(http::ContentLength);
  if (contentLenIt != headersMap.end()) {
    const std::string_view contentLenValue = contentLenIt->second;
    std::size_t declaredLen{};
    [[maybe_unused]] const auto ret =
        std::from_chars(contentLenValue.data(), contentLenValue.data() + contentLenValue.size(), declaredLen);
    assert(ret.ec == std::errc());  // Content-Length is guaranteed to be a valid integer by the HTTP parser

    return declaredLen >= streamingDecompressionThresholdBytes;
  }
  return false;
}

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)

// Decode a single contiguous compressed `src` with one decoder, either in one-shot mode or, when
// `useStreaming` is set, by feeding fixed-size chunks into a streaming context (bounded intermediate
// memory for large payloads). Appends the decompressed bytes to `dst` (cleared first). Returns false on
// decoder error or when more than maxPlainBytes are decoded. Shared by the inbound (server) request
// decompression and the outbound (client) response decompression so both stay in lock-step.
bool DecodeContiguous(auto& decoder, std::string_view src, bool useStreaming, const DecompressionConfig& cfg,
                      std::size_t maxPlainBytes, RawChars& dst) {
  dst.clear();
  if (!useStreaming) {
    return decoder.decompressFull(src, maxPlainBytes, cfg.decoderChunkSize, dst);
  }
  auto* ctx = decoder.makeContext();
  // The decompress body function cannot be called without body.
  assert(!src.empty());
  for (std::size_t processed = 0; processed < src.size();) {
    const std::size_t remaining = src.size() - processed;
    const std::size_t chunkLen = std::min<std::size_t>(cfg.decoderChunkSize, remaining);
    const std::string_view chunk(src.data() + processed, chunkLen);
    processed += chunkLen;
    // maxPlainBytes bounds the whole decoded body: each chunk gets what the previous ones left.
    if (!ctx->decompressChunk(chunk, processed == src.size(), maxPlainBytes - dst.size(), cfg.decoderChunkSize, dst)) {
      return false;
    }
  }
  return true;
}

#endif

constexpr CompressResponseResult ConvertEncoderResultErrorToCompressResponseResult(EncoderResult encoderResult) {
  assert(encoderResult.hasError());
  if (encoderResult.error() == EncoderResult::Error::NotEnoughCapacity) {
    return CompressResponseResult::ExceedsMaxRatio;
  }
  return CompressResponseResult::Error;
}

}  // namespace

CompressionState::CompressionState(const CompressionConfig& cfg) noexcept
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

EncoderResult CompressionState::encodeFull([[maybe_unused]] Encoding encoding, [[maybe_unused]] std::string_view data,
                                           [[maybe_unused]] std::size_t availableCapacity, [[maybe_unused]] char* buf) {
#ifdef AERONET_ENABLE_BROTLI
  if (encoding == Encoding::br) {
    return brotliEncoder.encodeFull(data, availableCapacity, buf);
  }
#endif
#ifdef AERONET_ENABLE_ZLIB
  if (encoding == Encoding::gzip) {
    return zlibEncoder.encodeFull(ZlibVariant::gzip, data, availableCapacity, buf);
  }
  if (encoding == Encoding::deflate) {
    return zlibEncoder.encodeFull(ZlibVariant::deflate, data, availableCapacity, buf);
  }
#endif
#ifdef AERONET_ENABLE_ZSTD
  if (encoding == Encoding::zstd) {
    return zstdEncoder.encodeFull(data, availableCapacity, buf);
  }
#endif
  return EncoderResult(EncoderResult::Error::CompressionError);
}

EncoderContext* CompressionState::makeContext([[maybe_unused]] Encoding encoding) {
#ifdef AERONET_ENABLE_BROTLI
  if (encoding == Encoding::br) {
    return brotliEncoder.makeContext();
  }
#endif
#ifdef AERONET_ENABLE_ZLIB
  if (encoding == Encoding::gzip) {
    return zlibEncoder.makeContext(ZlibVariant::gzip);
  }
  if (encoding == Encoding::deflate) {
    return zlibEncoder.makeContext(ZlibVariant::deflate);
  }
#endif
#ifdef AERONET_ENABLE_ZSTD
  if (encoding == Encoding::zstd) {
    return zstdEncoder.makeContext();
  }
#endif
  // should not happen
  return nullptr;
}

EncoderContext* CompressionState::context([[maybe_unused]] Encoding encoding) {
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
  // should not happen
  return nullptr;
}

namespace {

constexpr std::string_view kCRLFVaryAcceptEncodingLine =
    JoinStringView_v<http::CRLF, http::Vary, http::HeaderSep, http::AcceptEncoding>;

constexpr std::string_view kVarySepAcceptEncoding = JoinStringView_v<kVaryHeaderValueSep, http::AcceptEncoding>;

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

inline std::size_t MinEncodeChunkAllocSize([[maybe_unused]] Encoding encoding) {
#ifdef AERONET_ENABLE_BROTLI
  if (encoding == Encoding::br) {
    return BrotliEncoderMaxCompressedSize(1U);
  }
#endif
#ifdef AERONET_ENABLE_ZSTD
  if (encoding == Encoding::zstd) {
    // This function exists primarily for zstd - the minimum required output buffer size for streaming compression is
    // about ~128KB, which is much larger than the default 'initialCompressionBufferLimit'.
    return ZSTD_CStreamOutSize();
  }
#endif
#ifdef AERONET_ENABLE_ZLIB
  assert(encoding == Encoding::deflate || encoding == Encoding::gzip);
  return ZCompressBound(1U);
#else
  return 0;
#endif
}

}  // namespace

bool HttpCodec::IsContentTypeAllowed(const CompressionConfig& compressionConfig, const HttpMessage& msg) noexcept {
  return compressionConfig.contentTypeAllowList.empty() ||
         compressionConfig.allowListContainsMediaTypeOf(msg.headerValueOrEmpty(http::ContentType));
}

CompressResponseResult HttpCodec::TryCompressBody(CompressionState& compressionState, Encoding encoding,
                                                  HttpMessage& msg) {
  if (msg.hasContentEncoding()) {
    return CompressResponseResult::Uncompressed;
  }

  const std::size_t bodySz = msg.bodyInMemoryLength();
  const CompressionConfig& compressionConfig = *compressionState.pCompressionConfig;

  if (bodySz < compressionConfig.minBytes) {
    return CompressResponseResult::Uncompressed;
  }

  if (!IsContentTypeAllowed(compressionConfig, msg)) {
    return CompressResponseResult::Uncompressed;
  }

  // Sanity check: Content-Type header must be present to consider compression.
  // Normally handled by HttpResponse automatically when user adds a body.
  assert(msg.hasHeader(http::ContentType));

  // At this step, we will try the compression.
  // We have two cases - either the body is in the main buffer, or it is captured.

  // We will only commit compression if the configured compression ratio is satisfied.
  const std::size_t maxCompressedBytes = compressionConfig.maxCompressedBytes(bodySz);
  const std::string_view contentEncodingStr = GetEncodingStr(encoding);

  const bool hasBodyCaptured = msg.hasBodyCaptured();
  const auto trailersSz = msg.trailersSize();

  const char* pData = msg._data.data();
  const CompressionHeaders compressionHeaders =
      ScanCompressionHeaders(msg, pData, compressionConfig.addVaryAcceptEncodingHeader);
  const VaryResult& varyResult = compressionHeaders.vary;
  const std::size_t etagShift = compressionHeaders.strongETagPos == 0 ? 0UL : http::WeakETagPrefix.size();
  const bool hasVaryHeader = varyResult.valueFirst != varyResult.valueLast;
  const bool needVaryAcceptEncoding = !varyResult.notNeeded();
  const bool addVaryHeaderLine = varyResult.absent();
  const std::size_t additionalVaryLen =
      ComputeAdditionalVaryLength(needVaryAcceptEncoding, addVaryHeaderLine, hasVaryHeader);

  const std::size_t contentEncodingHeaderLineSz =
      http::HeaderSize(http::ContentEncoding.size(), contentEncodingStr.size());

  const bool sendContentLengthHeader = msg._opts.sendContentLengthHeader();

  // Compute offsets for the reserved tail (Content-Type + Content-Length + DoubleCRLF).
  const auto nDigitsMaxCompressedSize = ndigits(maxCompressedBytes);
  const std::size_t contentTypeLinePos = static_cast<std::size_t>(msg.getContentTypeHeaderLinePtr() - pData);
  const std::size_t oldDataSz = msg.wireDataSize();

  std::size_t contentTypeLineLen;
  if (sendContentLengthHeader) {
    const std::size_t contentLengthLinePos = static_cast<std::size_t>(msg.getLastHeaderLinePtr() - pData);

    contentTypeLineLen = contentLengthLinePos - contentTypeLinePos;
  } else {
    const std::size_t lastHeaderValueEndPos = static_cast<std::size_t>(msg.getLastHeaderValueEndPtr() - pData);
    contentTypeLineLen = lastHeaderValueEndPos - contentTypeLinePos;
  }

  static_assert(http::HeaderSize(http::ContentEncoding.size(), 1U) >= std::numeric_limits<std::size_t>::digits10 + 1,
                "headersShift cannot be negative for below logic");

  std::size_t headersShift = additionalVaryLen + contentEncodingHeaderLineSz + etagShift;

  if (sendContentLengthHeader) {
    const std::size_t contentLengthLineLen = http::HeaderSize(http::ContentLength.size(), ndigits(bodySz));
    const std::size_t upperContentLengthLineLen =
        http::HeaderSize(http::ContentLength.size(), nDigitsMaxCompressedSize);

    headersShift += upperContentLengthLineLen - contentLengthLineLen;
  }

  const std::size_t capturedTrailerGrowth = hasBodyCaptured ? trailersSz : 0UL;

  const std::size_t initialCompressionBufferLimit =
      std::max<std::size_t>(compressionConfig.initialCompressionBufferLimit, MinEncodeChunkAllocSize(encoding));
  const std::size_t initialCompressedBytes = std::min<std::size_t>(maxCompressedBytes, initialCompressionBufferLimit);
  const std::size_t initialNeededCapacity = oldDataSz + initialCompressedBytes + headersShift + capturedTrailerGrowth;

  // Extract embedded payload (if any) from the buffer before overwriting its storage.
  // It is put back if the compression is aborted, so that the response stays unmodified.
  struct PayloadRestorer {
    PayloadRestorer(HttpMessage& msg, HttpPayload payload) : msg(msg), payload(std::move(payload)) {}

    PayloadRestorer(const PayloadRestorer&) = delete;
    PayloadRestorer(PayloadRestorer&&) noexcept = delete;
    PayloadRestorer& operator=(const PayloadRestorer&) = delete;
    PayloadRestorer& operator=(PayloadRestorer&&) noexcept = delete;

    ~PayloadRestorer() {
      if (!payload.empty()) {
        // Compression aborted: the head is unchanged and the buffer never shrinks, so the payload goes back to its
        // original position without any reallocation.
        assert(msg._data.size() == msg.bodyStartPos());
        EmbeddedPayload::EmplaceNoRealloc(msg._data, std::move(payload));
        msg._opts.setHasEmbeddedPayload();
      }
    }

    HttpMessage& msg;
    HttpPayload payload;
  } payloadRestorer(msg, msg.releaseEmbeddedPayload());

  const HttpPayload& embeddedPayload = payloadRestorer.payload;

  // Reserve once (no realloc after we start reading internal body).
  // We reserve for:
  //   - worst-case tail growth (using current body digit count as upper bound)
  //   - temp compressed output (capped by maxCompressedBytes + 1)
  //   - final compressed output (capped by maxCompressedBytes)
  msg.reserve(initialNeededCapacity);

  // Note: we are before the finalization of the HttpResponse here, so there is no Transfer-Encoding: chunked case to
  // worry about (with tail \r\n0\r\n[trailers]\r\n). The trailers are directly after the body if they exist (not even a
  // CRLF splits the body and the first trailer line).
  const std::size_t bodyCompStartPos = oldDataSz + headersShift;
  char* pCompBody = msg._data.data() + bodyCompStartPos;

  std::string_view body;
  if (hasBodyCaptured) {
    body = embeddedPayload.view();
    body.remove_suffix(trailersSz);
  } else {
    body = msg.bodyInMemory();
  }

  std::size_t totalCompSize;
  if (maxCompressedBytes <= initialCompressionBufferLimit) {
    // For small max compressed size, we can just try to compress directly to the final position without streaming.
    const auto result = compressionState.encodeFull(encoding, body, maxCompressedBytes, pCompBody);
    if (result.hasError()) {
      // compression failed or did not fit in maxCompressedBytes - abort compression and leave the response unmodified.
      return ConvertEncoderResultErrorToCompressResponseResult(result);
    }
    totalCompSize = result.writtenIfNoError();
  } else {
    std::size_t totalCompCapa = std::min<std::size_t>(maxCompressedBytes, msg._data.capacity() - bodyCompStartPos);

    // For larger max compressed size, we will do streaming compression with flushes, to avoid compressing more than
    // needed in case the compression ratio is not satisfied.
    EncoderContext* encoder = compressionState.makeContext(encoding);
    totalCompSize = 0;

    // Grows the output room for the worst case of the next encoder call, up to maxCompressedBytes only: close to the
    // limit, the encoder gets the rest of the budget, and input left once it is used up means that the compressed body
    // exceeds it. Aborting as soon as the worst case exceeded the budget left most bodies of one to three chunks
    // uncompressed.
    const auto ensureEnoughCapacity = [&msg, &totalCompSize, &totalCompCapa, &pCompBody, &body, hasBodyCaptured,
                                       bodyCompStartPos, capturedTrailerGrowth,
                                       maxCompressedBytes](std::size_t neededCompressedChunkCapacity) {
      if (totalCompCapa < totalCompSize + neededCompressedChunkCapacity && totalCompCapa < maxCompressedBytes) {
        const std::size_t newTotalCompCapa =
            std::min(maxCompressedBytes, std::max(totalCompCapa * 2UL, totalCompSize + neededCompressedChunkCapacity));
        const std::size_t newTotalCapacity = bodyCompStartPos + newTotalCompCapa + capturedTrailerGrowth;

#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
        // Temporarily extend size to cover compressed output so reallocUp preserves it.
        const auto oldDataSz = msg._data.size();
        msg._data.setSize(bodyCompStartPos + totalCompSize);
#endif
        msg.reserve(newTotalCapacity);
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
        msg._data.setSize(oldDataSz);
#endif

        totalCompCapa = newTotalCompCapa;
        pCompBody = msg._data.data() + bodyCompStartPos;
        if (!hasBodyCaptured) {
          body = msg.bodyInMemory();
        }
      }
    };

    // Compress body in chunks, with flushes in between, to be able to stop as soon as we exceed maxCompressedBytes
    // without allocating too much memory at once for large bodies.
    for (std::size_t offsetBody = 0; offsetBody < bodySz;) {
      const std::size_t chunkSize = std::min<std::size_t>(bodySz - offsetBody, initialCompressionBufferLimit);

      ensureEnoughCapacity(encoder->minEncodeChunkCapacity(chunkSize));
      if (totalCompSize == totalCompCapa) {
        // The budget is used up with input left: end() would write more.
        return CompressResponseResult::ExceedsMaxRatio;
      }

      std::string_view chunk = body.substr(offsetBody, chunkSize);
      const auto result = encoder->encodeChunk(chunk, totalCompCapa - totalCompSize, pCompBody + totalCompSize);
      if (result.hasError()) {
        return ConvertEncoderResultErrorToCompressResponseResult(result);
      }
      // TODO: abort compression if compression ratio of first chunk is catastrophic?
      // Pre-sizing based on historical compression ratio per route?
      totalCompSize += result.writtenIfNoError();
      assert(totalCompSize <= maxCompressedBytes);
      // The rest of a chunk not consumed for lack of output room starts the next one, with more room.
      offsetBody += chunkSize - chunk.size();
    }

    // finalization flush(es)
    const auto endChunkSz = encoder->endChunkSize();
    while (true) {
      ensureEnoughCapacity(endChunkSz);
      const auto result = encoder->end(totalCompCapa - totalCompSize, pCompBody + totalCompSize);
      if (result.hasError()) {
        return ConvertEncoderResultErrorToCompressResponseResult(result);
      }
      const auto written = result.writtenIfNoError();
      if (written == 0) {
        // compression finished, we can proceed with this compressed body (meets compression ratio requirement)
        break;
      }
      totalCompSize += written;
      assert(totalCompSize <= maxCompressedBytes);
    }
  }

  // Compression succeeded at this point, we can start to move parts to their final position.
  // We will move the HttpResponse parts from backwards to avoid overwriting data, in this order:
  // 1) trailers
  // 2) compressed body (only if inlined, for captured bodies the compressed body is already in its final position)
  // 3) Double CRLF (before the compressed body)
  // 4) (optional) new Content-Length header line (with padding if needed)
  // 5) Content-Type header line (moved)
  // 6) new Content-Encoding header line (added)
  // 7) new Vary header line or value update (added or updated if needed)
  //
  //  For inline bodies:
  //   [headers][content-type][content-length][CRLF][uncompressed body][trailers][<headersShift>][compressed body]
  //  For captured bodies:
  //   [headers][content-type][content-length][CRLF][<headersShift>][compressed body]
  //   [uncompressed body][trailers]

  auto newBodyStartPos = msg.bodyStartPos() + headersShift;

  uint8_t nbDigitsContentLen = 0;
  uint8_t contentLengthPadding = 0;
  bool useLeadingZeroes = false;
  if (sendContentLengthHeader) {
    nbDigitsContentLen = ndigits(totalCompSize);
    contentLengthPadding = static_cast<uint8_t>(nDigitsMaxCompressedSize - nbDigitsContentLen);
    useLeadingZeroes = compressionConfig.useLeadingZeroesInContentLength;

    if (!useLeadingZeroes) {
      newBodyStartPos -= contentLengthPadding;
    }
  }

  // Buffer layout after trailers move to their final position:
  //  For inline bodies:
  //   [headers][content-type][content-length][CRLF][???][trailers][???][compressed body]
  //  For captured bodies:
  //   [headers][content-type][content-length][CRLF][<headersShift>][compressed body][trailers]
  //   [uncompressed body]

  // The trailers must follow the compressed body at its final position.

  // Move trailers first for inline bodies, since the inline source may overlap the final body area.
  const auto newTrailersPos = newBodyStartPos + totalCompSize;

  if (hasBodyCaptured) {
    if (!useLeadingZeroes && contentLengthPadding != 0) {
      // Captured bodies already have the compressed output in the reserved tail.
      // If the Content-Length field is compacted, move the compressed output backwards by the amount of removed
      // padding.
      std::memmove(msg._data.data() + newBodyStartPos, pCompBody, totalCompSize);
    }

    if (trailersSz != 0) {
      // Move trailers to their final position after the compressed body.
      Copy(embeddedPayload.view().data() + bodySz, trailersSz, msg._data.data() + newTrailersPos);
    }
  } else {
    if (trailersSz != 0) {
      std::memmove(msg._data.data() + newTrailersPos, msg.trailersFlatView().data(), trailersSz);
    }
    // Move body to its final position (after the reserved tail and after the potential headers inserted for Vary and
    // Content-Encoding).
    Copy(pCompBody, totalCompSize, msg._data.data() + newBodyStartPos);
  }

  // Update body start position to the new location.
  msg.setBodyStartPos(newBodyStartPos);

  // At this point, compressed body and trailers (if exist) are in their final position, but headers are not updated yet
  // (Content-Length, new headers for Content-Encoding and potentially Vary) and not moved yet to their final position.

  // Buffer layout after compressed body move to final position (only for inline bodies, for captured bodies the
  // compressed body is already in its final position):
  //  [headers][content-type][content-length][CRLF][<headersShift>][compressed body][trailers]

  // Write DoubleCRLF before the compressed body.
  char* pOut = msg._data.data() + newBodyStartPos - http::DoubleCRLF.size();
  CopyFixed<http::DoubleCRLF>(pOut);

  if (sendContentLengthHeader) {
    // Write new Content-Length, padded with zeroes as prefix if the number of chars of the actual compressed size is
    // smaller than the number of chars of the declared max compressed size (worst case). Example:
    //  'content-length: 0001234' if the actual compressed size is 1234 and uncompressed size had 7 digits.
    WriteUInt(pOut - nbDigitsContentLen, totalCompSize, nbDigitsContentLen);
    pOut -= nbDigitsContentLen;

    // Prefix with zeroes if needed. Width must match the reserved upper bound (nDigitsMaxCompressedSize),
    // the same value used to compute upperContentLengthLineLen / headersShift above.
    if (useLeadingZeroes && contentLengthPadding != 0) {
      std::memset(pOut - contentLengthPadding, '0', contentLengthPadding);
      pOut -= contentLengthPadding;
    }

    // Write '\r\nContent-Length: ' just before the new Content-Length value.
    CopyFixed<http::CRLFContentLengthHeaderSep>(pOut - http::CRLFContentLengthHeaderSep.size());
    pOut -= http::CRLFContentLengthHeaderSep.size();
  }

  // Write '\r\nContent-Type: XXXX' just before the Content-Length line.
  std::memmove(pOut - contentTypeLineLen, msg._data.data() + contentTypeLinePos, contentTypeLineLen);
  pOut -= contentTypeLineLen;

  // Write new '\r\nContent-Encoding: XXXX' header.
  WriteCRLFHeader(http::ContentEncoding, contentEncodingStr, pOut - contentEncodingHeaderLineSz);
  pOut -= contentEncodingHeaderLineSz;

  // Write '\r\nVary: Accept-Encoding' if needed.
  if (addVaryHeaderLine) {
    pOut -= kCRLFVaryAcceptEncodingLine.size();
    CopyFixed<kCRLFVaryAcceptEncodingLine>(pOut);
  }

  // The headers before the Content-Type line get the values inserted in them: ', Accept-Encoding' (or
  // 'Accept-Encoding' for an empty value) appended to an existing Vary header without Accept-Encoding, and the weak
  // prefix of a strong ETag. Both are before the Content-Type line, the body headers being the last ones.
  HeadInsertion insertions[2];
  std::size_t nbInsertions = 0;
  if (needVaryAcceptEncoding && !addVaryHeaderLine) {
    const std::string_view varyInsertion =
        hasVaryHeader ? kVarySepAcceptEncoding : kVarySepAcceptEncoding.substr(kVaryHeaderValueSep.size());
    insertions[nbInsertions++] = {.pos = varyResult.valueLast, .str = varyInsertion};
  }
  if (etagShift != 0) {
    insertions[nbInsertions++] = {.pos = compressionHeaders.strongETagPos, .str = http::WeakETagPrefix};
    if (nbInsertions == 2 && insertions[0].pos < insertions[1].pos) {
      std::swap(insertions[0], insertions[1]);
    }
  }
  assert(pOut == msg._data.data() + contentTypeLinePos + (addVaryHeaderLine ? 0UL : additionalVaryLen) + etagShift);
  InsertInHead(msg._data.data(), contentTypeLinePos, std::span<const HeadInsertion>(insertions, nbInsertions));

  // Finalize response metadata to reflect compression
  msg._data.setSize(newBodyStartPos + totalCompSize + trailersSz);
  payloadRestorer.payload.reset();

  return CompressResponseResult::Compressed;
}

RequestDecompressionResult HttpCodec::MaybeDecompressRequestBody(DecompressionState& decompressionState,
                                                                 const DecompressionConfig& decompressionConfig,
                                                                 HttpRequestView& request,
                                                                 RawChars& bodyAndTrailersBuffer, RawChars& tmpBuffer) {
  if (!decompressionConfig.enable) {
    return {};
  }

  SvToSvMap& headersMap = request._headers;
  const auto encodingHeaderIt = headersMap.find(http::ContentEncoding);
  if (encodingHeaderIt == headersMap.end()) {
    return {};
  }

  // Not body(), which would commit the handler to the aggregated access.
  std::string_view src = request._body;
  const std::size_t compressedSize = src.size();
  if (compressedSize > decompressionConfig.maxCompressedBytes) {
    return {.status = http::StatusCodePayloadTooLarge, .message = "Payload too large"};
  }

  RequestDecompressionResult res = DualBufferDecodeLoop(
      decompressionState,
      [useStreamingDecode =
           UseStreamingDecompression(headersMap, decompressionConfig.streamingDecompressionThresholdBytes),
       &src, &decompressionConfig](auto& decoder, RawChars& dst, std::size_t maxPlainBytes) {
        return DecodeContiguous(decoder, src, useStreamingDecode, decompressionConfig, maxPlainBytes, dst);
      },
      decompressionConfig, src, encodingHeaderIt->second, compressedSize, bodyAndTrailersBuffer, tmpBuffer);

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

bool HttpCodec::WillDecompress(const DecompressionConfig& decompressionConfig, const SvToSvMap& headersMap) {
  if (!decompressionConfig.enable) {
    return false;
  }
  const auto encodingHeaderIt = headersMap.find(http::ContentEncoding);
  if (encodingHeaderIt == headersMap.end()) {
    return false;
  }
  const std::string_view encodingStr = encodingHeaderIt->second;
  // Codings are compared exactly: the parsers lower-case received Content-Encoding values.
  assert(!LowerAsciiKey::ContainsUpperAscii(encodingStr));

  // Empty elements are ignored, as identity.
  for (http::HeaderValueReverseTokensIterator encIt(encodingStr); encIt.hasNext();) {
    const std::string_view encoding = encIt.next();
    if (!encoding.empty() && encoding != http::identity) {
      return true;
    }
  }
  return false;
}

RequestDecompressionResult HttpCodec::DecompressChunkedBody(DecompressionState& decompressionState,
                                                            const DecompressionConfig& decompressionConfig,
                                                            HttpRequestView& request,
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
      [&src, compressedChunks, decoderChunkSize = decompressionConfig.decoderChunkSize, firstStage = true](
          auto& decoder, RawChars& dst, std::size_t maxPlainBytes) mutable {
        dst.clear();

        if (firstStage) {
          firstStage = false;
          auto* ctx = decoder.makeContext();
          for (std::size_t idx = 0; idx < compressedChunks.size(); ++idx) {
            const bool lastChunk = (idx == compressedChunks.size() - 1);
            // maxPlainBytes bounds the whole decoded body: each chunk gets what the previous ones left.
            if (!ctx->decompressChunk(compressedChunks[idx], lastChunk, maxPlainBytes - dst.size(), decoderChunkSize,
                                      dst)) {
              return false;
            }
          }
          return true;
        }
        return decoder.decompressFull(src, maxPlainBytes, decoderChunkSize, dst);
      },
      decompressionConfig, src, encodingHeaderIt->second, compressedSize, bodyAndTrailersBuffer, tmpBuffer);
  if (res.status != http::StatusCodeOK) {
    return res;
  }

  request._body =
      FinalizeDecompressedBody(headersMap, encodingHeaderIt, src, additionalCapacity, bodyAndTrailersBuffer);

  return {};
}

RequestDecompressionResult HttpCodec::DecompressFullBody(DecompressionState& decompressionState,
                                                         const DecompressionConfig& decompressionConfig,
                                                         std::string_view contentEncoding,
                                                         std::string_view compressedBody, RawChars& outBuffer,
                                                         RawChars& tmpBuffer, std::string_view& outDecompressed) {
  // Default: nothing decoded yet (identity / pass-through result is the input itself).
  outDecompressed = compressedBody;

  if (compressedBody.size() > decompressionConfig.maxCompressedBytes) {
    return {.status = http::StatusCodePayloadTooLarge, .message = "Payload too large"};
  }

  std::string_view src = compressedBody;
  const bool useStreaming = compressedBody.size() >= decompressionConfig.streamingDecompressionThresholdBytes;

  RequestDecompressionResult res = DualBufferDecodeLoop(
      decompressionState,
      [useStreaming, &src, &decompressionConfig](auto& decoder, RawChars& dst, std::size_t maxPlainBytes) {
        return DecodeContiguous(decoder, src, useStreaming, decompressionConfig, maxPlainBytes, dst);
      },
      decompressionConfig, src, contentEncoding, compressedBody.size(), outBuffer, tmpBuffer);

  if (res.status == http::StatusCodeNotModified) {
    // Only identity codings were present: nothing was decoded, deliver the body verbatim.
    res.status = http::StatusCodeOK;
  }
  // After the loop `src` points to the final decoded buffer (or the input when only identity was present).
  outDecompressed = src;
  return res;
}

}  // namespace aeronet
