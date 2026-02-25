#include "aeronet/http-response.hpp"

#include <algorithm>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/concatenated-headers.hpp"
#include "aeronet/direct-compression-mode.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/file.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/toupperlower.hpp"

#ifndef NDEBUG
#include "aeronet/reserved-headers.hpp"
#endif

namespace aeronet {

namespace {
// The RFC does not specify a maximum length for the reason phrase,
// but in practice it should be reasonable. It's not really used by clients,
// as they mostly rely on the status code instead.
constexpr std::string_view::size_type kMaxReasonLength = 1024;

// Number of digits in the status code (3 digits).
constexpr std::size_t kStatusCodeLen = 3U;

// Date header will always be present at headersStartPos.
constexpr std::size_t kDateHeaderLenWithCRLF = HttpResponse::HeaderSize(http::Date.size(), RFC7231DateStrLen);

constexpr std::size_t kStatusLineMinLenWithoutCRLF = http::HTTP10Sv.size() + 1U + kStatusCodeLen;

// Initial size of the HttpResponse internal buffer, including the status line, Date header and DoubleCRLF.
constexpr std::size_t kHttpResponseInitialSize =
    kStatusLineMinLenWithoutCRLF + kDateHeaderLenWithCRLF + http::DoubleCRLF.size();

static_assert(kHttpResponseInitialSize <= HttpResponse::kHttpResponseMinInitialCapacity,
              "Initial size should be less than or equal to min initial capacity");

constexpr std::string_view AdjustReasonLen(std::string_view reason) {
  if (reason.size() > kMaxReasonLength) [[unlikely]] {
    log::warn("Provided reason is too long ({} bytes), truncating it to {} bytes", reason.length(), kMaxReasonLength);
    reason.remove_suffix(reason.size() - kMaxReasonLength);
  }
  return reason;
}

constexpr auto kInitialBodyStart = kHttpResponseInitialSize;

constexpr void InitData(char* data) {
  data[http::HTTP10Sv.size()] = ' ';
  data[HttpResponse::kReasonBeg] = '\n';  // marker for no reason
#ifndef NDEBUG
  // In debug, this allows for easier inspection of the response data before finalization.
  // In release, it's not needed because the final HTTP version and date will be written at finalization step.
  http::HTTP_1_1.writeFull(data);
  WriteCRLFDateHeader(SysClock::now(), data + kStatusLineMinLenWithoutCRLF);
#endif
}

constexpr std::size_t NeededBodyHeadersSize(std::size_t bodySize, std::size_t contentTypeSize) {
  if (bodySize == 0) {
    return 0;
  }
  return HttpResponse::HeaderSize(http::ContentType.size(), contentTypeSize) +
         HttpResponse::HeaderSize(http::ContentLength.size(), nchars(bodySize));
}

constexpr std::string_view kTrailerValueSep = ", ";

constexpr std::string_view kTransferEncodingChunkedCRLF =
    JoinStringView_v<http::TransferEncoding, http::HeaderSep, http::chunked, http::CRLF>;
constexpr std::string_view kEndChunkedBody = "\r\n0\r\n";

// Returns the size difference between the new Transfer-Encoding: chunked header and Content-Length header.
// For very large payloads, this can be negative, as Content-Length can be larger than
// Transfer-Encoding: chunked.
constexpr int64_t TransferEncodingHeaderSizeDiff(std::uint8_t nCharsBodyLen) {
  const auto oldContentLengthHeaderSize = HttpResponse::HeaderSize(http::ContentLength.size(), nCharsBodyLen);

  return static_cast<int64_t>(kTransferEncodingChunkedCRLF.size()) - static_cast<int64_t>(oldContentLengthHeaderSize);
}

constexpr char* ReplaceContentLengthWithTransferEncoding(char* insertPtr, std::string_view newTrailersFlatView,
                                                         bool addTrailerHeader) {
  insertPtr = insertPtr + http::CRLF.size();

  // If adding Trailer header, write it now
  if (addTrailerHeader) {
    insertPtr = Append(http::Trailer, insertPtr);
    insertPtr = Append(http::HeaderSep, insertPtr);

    bool isFirst = true;
    for (const auto& [name, value] : HeadersView(newTrailersFlatView)) {
      if (isFirst) {
        isFirst = false;
      } else {
        insertPtr = Append(kTrailerValueSep, insertPtr);
      }
      insertPtr = Append(name, insertPtr);
    }
    insertPtr = Append(http::CRLF, insertPtr);
  }

  // Write Transfer-Encoding: chunked header
  insertPtr = Append(kTransferEncodingChunkedCRLF, insertPtr);

  return insertPtr - http::CRLF.size();
}

constexpr std::string_view CheckContentType(bool isBodyEmpty, std::string_view contentType) {
  contentType = TrimOws(contentType);
  if (!isBodyEmpty && (contentType.size() < http::ContentTypeMinLen || !http::IsValidHeaderValue(contentType)))
      [[unlikely]] {
    throw std::invalid_argument("HTTP content-type header value is invalid");
  }
  return contentType;
}

constexpr void CheckContentTypeLengthEncoding(std::string_view headerName, bool hasBody) {
  // Fast path: most headers don't start with 'c' or 'C'
  assert(!headerName.empty());
  if (headerName[0] != 'c' && headerName[0] != 'C') {
    return;
  }
  if (headerName.size() != http::ContentType.size() && headerName.size() != http::ContentLength.size()) {
    if (hasBody && CaseInsensitiveEqual(headerName, http::ContentEncoding)) [[unlikely]] {
      throw std::logic_error("content-encoding cannot be changed with body already set");
    }
    return;
  }

  // Second fast path: check exact length (Content-Type is 12 chars, Content-Length is 14 chars)
  if (CaseInsensitiveEqual(headerName, http::ContentType)) [[unlikely]] {
    throw std::invalid_argument("content-type header should be set with the body");
  }
  if (CaseInsensitiveEqual(headerName, http::ContentLength)) [[unlikely]] {
    throw std::invalid_argument("content-length header should be set with the body");
  }
}

constexpr void CheckConcatenatedHeaders(std::string_view concatenatedHeaders) {
  const char* first = concatenatedHeaders.data();
  const char* last = first + concatenatedHeaders.size();

  while (first < last) {
    const char* headerNameEnd = std::search(first, last, http::HeaderSep.begin(), http::HeaderSep.end());
    if (headerNameEnd == last) {
      throw std::invalid_argument("header missing http::HeaderSep separator in concatenated headers");
    }

    std::string_view headerName(first, headerNameEnd);
    if (!http::IsValidHeaderName(headerName)) {
      throw std::invalid_argument("Invalid header name in concatenated headers");
    }
    first += headerName.size() + http::HeaderSep.size();

    const char* endLine = std::search(first, last, http::CRLF.begin(), http::CRLF.end());
    if (endLine == last) {
      throw std::invalid_argument("header missing CRLF terminator in concatenated headers");
    }

    std::string_view headerValue(first, endLine);
    if (!http::IsValidHeaderValue(headerValue)) {
      throw std::invalid_argument("Invalid header value in concatenated headers");
    }

    first = endLine + http::CRLF.size();
  }
}

}  // namespace

HttpResponse::HttpResponse(http::StatusCode code, std::string_view body, std::string_view contentType) : _posBitmap() {
  contentType = CheckContentType(body.empty(), contentType);
  _data.reserve(kHttpResponseInitialSize + NeededBodyHeadersSize(body.size(), contentType.size()) + body.size());
  InitData(_data.data());
  status(code);
  setHeadersStartPos(static_cast<std::uint16_t>(kStatusLineMinLenWithoutCRLF));
  if (body.empty()) {
    setBodyStartPos(kInitialBodyStart);
    Copy(http::DoubleCRLF, _data.data() + kInitialBodyStart - http::DoubleCRLF.size());
    _data.setSize(kInitialBodyStart);
  } else {
    char* insertPtr = WriteCRLFHeader(http::ContentType, contentType,
                                      _data.data() + kStatusLineMinLenWithoutCRLF + kDateHeaderLenWithCRLF);
    insertPtr = WriteCRLFHeader(http::ContentLength, body.size(), insertPtr);
    insertPtr = Append(http::DoubleCRLF, insertPtr);
    const auto bodyStartPos = static_cast<std::uint64_t>(insertPtr - _data.data());
    setBodyStartPos(bodyStartPos);
    Copy(body, insertPtr);
    _data.setSize(bodyStartPos + body.size());
  }
}

HttpResponse::HttpResponse(std::size_t additionalCapacity, http::StatusCode code)
    : _data(kHttpResponseInitialSize + additionalCapacity), _posBitmap() {
  InitData(_data.data());
  status(code);
  setHeadersStartPos(static_cast<std::uint16_t>(kStatusLineMinLenWithoutCRLF));
  setBodyStartPos(kInitialBodyStart);
  Copy(http::DoubleCRLF, _data.data() + kInitialBodyStart - http::DoubleCRLF.size());
  _data.setSize(kInitialBodyStart);
}

HttpResponse::HttpResponse(std::size_t additionalCapacity, http::StatusCode code, std::string_view concatenatedHeaders,
                           std::string_view body, std::string_view contentType, Check check)
    : _posBitmap() {
  contentType = CheckContentType(body.empty(), contentType);
  if (check == Check::Yes) {
    CheckConcatenatedHeaders(concatenatedHeaders);
  }
  _data.reserve(kHttpResponseInitialSize + concatenatedHeaders.size() +
                NeededBodyHeadersSize(body.size(), contentType.size()) + body.size() + additionalCapacity);
  InitData(_data.data());
  status(code);
  setHeadersStartPos(static_cast<std::uint16_t>(kStatusLineMinLenWithoutCRLF));
  std::size_t bodyStartPos = kInitialBodyStart - http::CRLF.size();
  if (!concatenatedHeaders.empty()) {
    char* insertPtr = _data.data() + kHttpResponseInitialSize - http::DoubleCRLF.size();
    insertPtr = Append(http::CRLF, insertPtr);
    Copy(concatenatedHeaders, insertPtr);
    bodyStartPos += concatenatedHeaders.size();
  }
  if (body.empty()) {
    bodyStartPos += http::CRLF.size();
  } else {
    char* insertPtr = WriteCRLFHeader(http::ContentType, contentType, _data.data() + bodyStartPos - http::CRLF.size());
    insertPtr = WriteCRLFHeader(http::ContentLength, body.size(), insertPtr);
    bodyStartPos = static_cast<std::uint64_t>(insertPtr + http::DoubleCRLF.size() - _data.data());
    Copy(body, insertPtr + http::DoubleCRLF.size());
  }
  Copy(http::DoubleCRLF, _data.data() + bodyStartPos - http::DoubleCRLF.size());
  setBodyStartPos(bodyStartPos);
  _data.setSize(bodyStartPos + body.size());
}

std::size_t HttpResponse::bodyLength() const noexcept {
  if (const auto* pFilePayload = filePayloadPtr(); pFilePayload != nullptr) {
    return static_cast<std::size_t>(pFilePayload->length);
  }
  return bodyInMemoryLength();
}

HttpResponse& HttpResponse::status(http::StatusCode statusCode) & {
  if (statusCode < 100 || statusCode > 999) [[unlikely]] {
    throw std::invalid_argument("Invalid HTTP status code, should be 3 digits");
  }
  write3(_data.data() + kStatusCodeBeg, statusCode);
  return *this;
}

HttpResponse& HttpResponse::reason(std::string_view newReason) & {
  newReason = AdjustReasonLen(newReason);
  auto oldReasonSz = reasonLength();
  int32_t diff = static_cast<int32_t>(newReason.size()) - static_cast<int32_t>(oldReasonSz);
  if (diff == 0) {
    if (!newReason.empty()) {
      Copy(newReason, _data.data() + kReasonBeg);
    }
    return *this;
  }
  if (diff > 0) {
    if (oldReasonSz == 0) {
      ++diff;  // for the space before first reason char
    }
    _data.ensureAvailableCapacityExponential(static_cast<uint64_t>(diff));
  } else if (newReason.empty()) {
    --diff;  // remove the space that previously separated status and reason
  }
  const std::string_view oldReason = reason();
  char* orig = _data.data() + kReasonBeg + oldReasonSz;
  if (oldReason.empty()) {
    --orig;  // point to the CR (space placeholder location)
  }
  std::memmove(
      orig + diff, orig,
      _data.size() - kStatusCodeBeg - kStatusCodeLen - oldReasonSz - static_cast<uint32_t>(!oldReason.empty()));
  adjustBodyStart(diff);
  adjustHeadersStart(diff);
  if (newReason.empty()) {
    _data[kReasonBeg] = '\n';  // needed marker for empty reason
  } else {
    _data[kReasonBeg - 1UL] = ' ';
    Copy(newReason, _data.data() + kReasonBeg);
  }
  _data.adjustSize(diff);
  return *this;
}

namespace {

constexpr void SetBodyEnsureNoTrailers(std::size_t trailerLen) {
  if (trailerLen != 0) [[unlikely]] {
    throw std::logic_error("Cannot set body after the first trailer");
  }
}

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
constexpr std::string_view kVaryHeaderValueSep = ", ";

bool VaryContainsAcceptEncodingToken(std::string_view value) {
  for (http::HeaderValueReverseTokensIterator<','> it(value); it.hasNext();) {
    const std::string_view token = it.next();
    if (token == "*" || CaseInsensitiveEqual(token, http::AcceptEncoding)) {
      return true;
    }
  }
  return false;
}
#endif

struct HeaderSearchResult {
  // if nullptr, not found. It's different from valueBegin == valueEnd, which means found but empty value.
  const char* valueBegin = nullptr;
  const char* valueEnd = nullptr;
};

// Performs a linear search for the header 'key' in the headers block defined by flatHeaders.
// flatHeaders should start at the start of the first header, without any leading CRLF.
// flatHeaders should end immediately after the last header CRLF.
// Returns {nullptr, nullptr} if the header is not found, or a value {valueFirst, valueLast} (may be empty) otherwise.
constexpr HeaderSearchResult HeadersLinearSearch(std::string_view flatHeaders, std::string_view key) noexcept {
  const char* endKey = key.end();
  const char* headersBeg = flatHeaders.data();
  const char* headersEnd = headersBeg + flatHeaders.size();

  while (headersBeg < headersEnd) {
    // Perform an inplace case-insensitive 'starts_with' algorithm
    const char* begKey = key.begin();
    for (; *headersBeg != ':' && begKey != endKey; ++headersBeg, ++begKey) {
      if (tolower(*headersBeg) != tolower(*begKey)) {
        break;
      }
    }

    const char* nextCRLF =
        std::search(headersBeg + http::HeaderSep.size(), headersEnd, http::CRLF.begin(), http::CRLF.end());

    if (*headersBeg == ':' && begKey == endKey) {
      // Found the header we are looking for
      return {headersBeg + http::HeaderSep.size(), nextCRLF};
    }

    // Not the header we are looking for - move headersBeg to next header
    headersBeg = nextCRLF + http::CRLF.size();
  }
  return {};
}

// Performs a linear search in reverse for the header 'key' in the headers block defined by flatHeaders.
// flatHeaders should start at the start of the first header, without any leading CRLF.
// flatHeaders should end immediately after the last header CRLF.
// Returns {nullptr, nullptr} if the header is not found, or a value {valueFirst, valueLast} (may be empty) otherwise.
constexpr HeaderSearchResult HeadersReverseLinearSearch(std::string_view flatHeaders, std::string_view key) noexcept {
  if (key.empty()) {
    return {};
  }
  const char* first = flatHeaders.data();
  const char* last = first + flatHeaders.size();

  while (first < last) {
    last -= http::CRLF.size();

    const char* endValue = last;

    while (*last != '\n') {
      --last;
    }

    const char* currentBegKey = ++last;

    const char* begValue =
        std::search(currentBegKey, endValue, http::HeaderSep.begin(), http::HeaderSep.end()) + http::HeaderSep.size();

    const char* currentEndKey = begValue - http::HeaderSep.size();

    if (CaseInsensitiveEqual(std::string_view(currentBegKey, currentEndKey), key)) {
      // Found the header we are looking for
      return {begValue, endValue};
    }
  }
  return {};
}

}  // namespace

HttpResponse& HttpResponse::header(std::string_view key, std::string_view value) & {
  const auto [first, last] = HeadersLinearSearch(headersFlatView(), key);
  if (first == nullptr) {
    headerAddLine(key, value);
    return *this;
  }

  CheckContentTypeLengthEncoding(key, hasBody());

  overrideHeaderUnchecked(first, last, value);
  return *this;
}

void HttpResponse::setBodyHeaders(std::string_view contentTypeValue, std::size_t newBodySize, BodySetContext context) {
  SetBodyEnsureNoTrailers(trailersSize());
  contentTypeValue = CheckContentType(newBodySize == 0, contentTypeValue);

  const auto oldBodyLen = bodyLength();
  const bool hadBody = oldBodyLen != 0 || _opts.isAutomaticDirectCompression();
  if (newBodySize == 0) {
    if (hadBody) {
      removeBodyAndItsHeaders();
    }
  } else {
    const auto newBodyLenCharVec = IntegralToCharVector(newBodySize);
    const auto newContentTypeHeaderSize = HeaderSize(http::ContentType.size(), contentTypeValue.size());
    const auto newContentLengthHeaderSize = HeaderSize(http::ContentLength.size(), newBodyLenCharVec.size());
    const bool setInlineBody = context == BodySetContext::Inline && !isHead();

    std::size_t neededNewSize = newContentTypeHeaderSize + newContentLengthHeaderSize;

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    const bool hadDirectCompression = _opts.isAutomaticDirectCompression();
    // We do NOT use direct compression for captured bodies - they will be compressed at finalization if needed.
    // We also do not activate direct compression for HEAD responses to avoid extra CPU work (see docs/FEATURES.md).
    // Note that this behavior is not strictly respecting RFC 9110 in this aspect. This could be configurable in the
    // future.
    const bool tryCompression = setInlineBody && (!hasContentEncoding() || hadDirectCompression) &&
                                _opts.directCompressionPossible(newBodySize, contentTypeValue);
    const bool addEncodingHeaders = tryCompression && !hasContentEncoding();

    // invariant: if we had direct compression, we must have had content encoding, because it's forbidden to remove
    // content encoding header when body is set.
    assert(!hadDirectCompression || hasContentEncoding());

    const bool removeEncodingHeaders = hadDirectCompression && !tryCompression;

    bool addVaryHeader = false;
    bool appendVaryValue = false;
    if (addEncodingHeaders) {
      neededNewSize += HeaderSize(http::ContentEncoding.size(), GetEncodingStr(_opts._pickedEncoding).size());
      if (_opts.isAddVaryAcceptEncoding()) {
        const auto [varyFirst, varyLast] = HeadersLinearSearch(headersFlatView(), http::Vary);
        if (varyFirst == nullptr) {
          addVaryHeader = true;
          neededNewSize += HeaderSize(http::Vary.size(), http::AcceptEncoding.size());
        } else if (!VaryContainsAcceptEncodingToken(std::string_view(varyFirst, varyLast))) {
          appendVaryValue = true;
          neededNewSize += http::AcceptEncoding.size() + kVaryHeaderValueSep.size();
        }
      }
    }
    // do not remove the space taken by content encoding and vary headers when removing content encoding to keep code
    // simple, but could be added if we want to be more aggressive on saving space when switching from compressed to
    // uncompressed response

    const auto adjustEncodingHeaders = [this, addEncodingHeaders, removeEncodingHeaders, addVaryHeader,
                                        appendVaryValue]() {
      if (addEncodingHeaders) {
        if (addVaryHeader) {
          headerAddLineUnchecked(http::Vary, http::AcceptEncoding);
        } else if (appendVaryValue) {
          headerAppendValue(http::Vary, http::AcceptEncoding, kVaryHeaderValueSep);
        }
        headerAddLineUnchecked(http::ContentEncoding, GetEncodingStr(_opts._pickedEncoding));
        _opts.setHasContentEncoding(true);
        _opts.setAutomaticDirectCompression(true);
      } else if (removeEncodingHeaders) {
        headerRemoveLine(http::ContentEncoding);
        _opts.setHasContentEncoding(false);
        _opts.setAutomaticDirectCompression(false);
        if (_opts.isAddVaryAcceptEncoding()) {
          headerRemoveValue(http::Vary, http::AcceptEncoding);
        }
      }
    };
#endif

    // only reserve body size if not captured (so inline) and not head
    if (setInlineBody) {
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
      if (tryCompression) {
        auto& compressionState = *_opts._pCompressionState;
        auto& encoderCtx = *compressionState.makeContext(_opts._pickedEncoding);

        neededNewSize += encoderCtx.minEncodeChunkCapacity(newBodySize);
      } else {
#endif
        neededNewSize += newBodySize;
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
      }
#endif
    }

    char* insertPtr;
    if (!hadBody) {
      _data.ensureAvailableCapacityExponential(neededNewSize);
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
      adjustEncodingHeaders();
#endif
      insertPtr = WriteHeader(http::ContentType, contentTypeValue, _data.data() + bodyStartPos() - http::CRLF.size());
    } else {
      const auto bodyStart = bodyStartPos();
      const auto oldContentTypeAndLengthSize =
          bodyStart - static_cast<std::size_t>(getContentTypeHeaderLinePtr() - _data.data()) - http::DoubleCRLF.size();
      const std::size_t oldBodyLenInlined = internalBodyAndTrailersLen();

      if (neededNewSize > oldContentTypeAndLengthSize + oldBodyLenInlined) {
        _data.ensureAvailableCapacityExponential(neededNewSize - oldContentTypeAndLengthSize - oldBodyLenInlined);
      }
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
      // tell adjustEncodingHeaders that body is empty to avoid moving memory around
      setBodyStartPos(bodyStart);
      _data.setSize(bodyStart);
      adjustEncodingHeaders();
#endif
      insertPtr = Append(contentTypeValue, getContentTypeValuePtr());
    }
    insertPtr = WriteCRLFHeader(http::ContentLength, std::string_view(newBodyLenCharVec), insertPtr);
    insertPtr = Append(http::DoubleCRLF, insertPtr);
    const auto newBodyStartPos = static_cast<std::uint64_t>(insertPtr - _data.data());
    setBodyStartPos(newBodyStartPos);
    _data.setSize(newBodyStartPos);
  }
}

void HttpResponse::setBodyInternal(std::string_view newBody) {
  if (isHead()) {
    return;
  }

  const auto bodySz = internalBodyAndTrailersLen();

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  if (_opts.isAutomaticDirectCompression()) {
    assert(!newBody.empty());

    _data.setSize(bodyStartPos());

    const auto written = appendEncodedInlineOrThrow(newBody);

    _data.setSize(_data.size() + written);
    return;
  }
#endif

  const int64_t diff = static_cast<int64_t>(newBody.size()) - static_cast<int64_t>(bodySz);

  // Capacity should already have been ensured in setBodyHeaders
  assert(static_cast<int64_t>(_data.size()) + diff >= 0);
  assert(static_cast<uint64_t>(static_cast<int64_t>(_data.size()) + diff) <= _data.capacity());

  // should not point to internal memory
  assert(newBody.empty() || newBody.data() <= _data.data() || newBody.data() > _data.data() + _data.size());

  _data.adjustSize(diff);
  if (!newBody.empty()) {
    Copy(newBody, _data.data() + bodyStartPos());
  }
}

HttpResponse& HttpResponse::bodyAppend(std::string_view body, std::string_view contentType) & {
  if (!body.empty()) {
    if (hasBodyFile()) [[unlikely]] {
      throw std::logic_error("Cannot append to a captured file body");
    }

    const auto oldBodyLen = bodyLength();
    const bool hadBody = oldBodyLen != 0 || _opts.isAutomaticDirectCompression();
    if (!hadBody) {
      if (contentType.empty()) {
        contentType = http::ContentTypeTextPlain;
      }
      return this->body(body, contentType);
    }

    bodyPrecheckContentType(contentType);

    const auto newBodyLen = oldBodyLen + body.size();
    const auto nbCharsNewBodyLen = nchars(newBodyLen);
    const bool capturedBody = hasBodyCaptured();

    const uint8_t nCharsOldBodyLen = nchars(oldBodyLen);
    int64_t neededCapacity = static_cast<int64_t>(nbCharsNewBodyLen) - static_cast<int64_t>(nCharsOldBodyLen);
    if (!capturedBody) {
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
      if (_opts.isAutomaticDirectCompression()) {
        auto& compressionState = *_opts._pCompressionState;
        auto& encoderCtx = *compressionState.context(_opts._pickedEncoding);
        neededCapacity += static_cast<int64_t>(encoderCtx.minEncodeChunkCapacity(body.size()));
      } else {
        neededCapacity += static_cast<int64_t>(body.size());
      }
#else
      neededCapacity += static_cast<int64_t>(body.size());
#endif
    }
    if (!contentType.empty()) {
      char* pContentTypeValuePtr = getContentTypeValuePtr();
      const auto it =
          std::search(pContentTypeValuePtr, _data.data() + _data.size(), http::CRLF.begin(), http::CRLF.end());
      assert(it != _data.data() + _data.size());
      const std::size_t oldContentTypeValueSize = static_cast<std::size_t>(it - pContentTypeValuePtr);
      neededCapacity += static_cast<int64_t>(contentType.size()) - static_cast<int64_t>(oldContentTypeValueSize);
    }

    _data.ensureAvailableCapacityExponential(neededCapacity);

    if (!contentType.empty()) {
      replaceHeaderValueNoRealloc(getContentTypeValuePtr(), contentType);
    }
    replaceHeaderValueNoRealloc(getContentLengthValuePtr(), std::string_view(IntegralToCharVector(newBodyLen)));

    if (isHead()) {
      assert(!capturedBody || _payloadVariant.isSizeOnly());
      setHeadSize(newBodyLen);
    } else if (capturedBody) {
      _payloadVariant.append(body);
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    } else if (_opts.isAutomaticDirectCompression()) {
      _data.addSize(appendEncodedInlineOrThrow(body));
#endif
    } else {
      _data.unchecked_append(body);
    }
  }
  return *this;
}

HttpResponse& HttpResponse::file(File fileObj, std::size_t offset, std::size_t length, std::string_view contentType) & {
  if (!fileObj) {
    throw std::invalid_argument("file requires an opened file");
  }
  const std::size_t fileSize = fileObj.size();
  if (fileSize < offset) {
    throw std::invalid_argument("file offset exceeds file size");
  }
  const std::size_t resolvedLength = length == 0 ? (fileSize - offset) : length;
  if (fileSize < offset + resolvedLength) {
    throw std::invalid_argument("file length exceeds file size");
  }
  if (contentType.empty()) {
    contentType = fileObj.detectedContentType();
  }

  // If file is empty, we emit on purpose Content-Length: 0 and no body.
  // This is to distinguish an empty file response from a response with no body at all.
  // This is valid per HTTP semantics.
  setBodyHeaders(contentType, std::max(1UL, resolvedLength), BodySetContext::Captured);
  if (resolvedLength == 0) {
    // we need to reset the '1' char that was written above
    *getContentLengthValuePtr() = '0';
  }

  setBodyInternal(std::string_view());
  _payloadVariant = HttpPayload(FilePayload(std::move(fileObj), offset, resolvedLength));
  return *this;
}

const File* HttpResponse::file() const noexcept {
  const auto* pFilePayload = _payloadVariant.getIfFilePayload();
  return pFilePayload == nullptr ? nullptr : &pFilePayload->file;
}

std::string_view HttpResponse::bodyInMemory() const noexcept {
  auto ret = hasBodyCaptured() ? _payloadVariant.view() : std::string_view{_data.begin() + bodyStartPos(), _data.end()};
  ret.remove_suffix(trailersSize());
  return ret;
}

bool HttpResponse::hasTrailer(std::string_view key) const noexcept {
  return HeadersLinearSearch(trailersFlatView(), key).valueBegin != nullptr;
}

std::optional<std::string_view> HttpResponse::trailerValue(std::string_view key) const noexcept {
  const auto [first, last] = HeadersLinearSearch(trailersFlatView(), key);
  if (first == nullptr) {
    return std::nullopt;
  }
  return std::string_view(first, last);
}

bool HttpResponse::hasHeader(std::string_view key) const noexcept {
  return HeadersLinearSearch(headersFlatView(), key).valueBegin != nullptr;
}

std::optional<std::string_view> HttpResponse::headerValue(std::string_view key) const noexcept {
  const auto [first, last] = HeadersLinearSearch(headersFlatView(), key);
  if (first == nullptr) {
    return std::nullopt;
  }
  return std::string_view(first, last);
}

[[nodiscard]] std::string_view HttpResponse::headerValueOrEmpty(std::string_view key) const noexcept {
  const auto [first, last] = HeadersLinearSearch(headersFlatView(), key);
  return {first, last};
}

HttpResponse& HttpResponse::headerAddLine(std::string_view key, std::string_view value) & {
  assert(!CaseInsensitiveEqual(key, http::Date));
  if (!http::IsValidHeaderName(key)) [[unlikely]] {
    throw std::invalid_argument("HTTP header name is invalid");
  }
  value = TrimOws(value);
  if (!http::IsValidHeaderValue(value)) [[unlikely]] {
    throw std::invalid_argument("HTTP header value is invalid");
  }
  CheckContentTypeLengthEncoding(key, hasBody());

  headerAddLineUnchecked(key, value);

  if (CaseInsensitiveEqual(key, http::ContentEncoding)) {
    _opts.setHasContentEncoding(true);
  }

  return *this;
}

void HttpResponse::headerAddLineUnchecked(std::string_view key, std::string_view value) {
  const std::size_t headerLineSize = HeaderSize(key.size(), value.size());

  _data.ensureAvailableCapacityExponential(headerLineSize);

  char* insertPtr = _data.data() + bodyStartPos() - http::DoubleCRLF.size();

  const auto bodySz = bodyLength();

  if (bodySz == 0) {
    Copy(http::DoubleCRLF, insertPtr + headerLineSize);
  } else {
    // We want to keep Content-Type and Content-Length together with the body (we use this property for optimization)
    // so we insert new headers before them. Of course, this code takes time, but it should be rare to add headers
    // after setting the body, so we can consider this as a 'slow' path.
    insertPtr = getContentTypeHeaderLinePtr();
    std::memmove(insertPtr + headerLineSize, insertPtr, static_cast<std::size_t>(_data.end() - insertPtr));
  }
  WriteCRLFHeader(key, value, insertPtr);
  _data.addSize(headerLineSize);

  adjustBodyStart(static_cast<int64_t>(headerLineSize));
}

void HttpResponse::overrideHeaderUnchecked(const char* oldValueFirst, const char* oldValueLast,
                                           std::string_view newValue) {
  newValue = TrimOws(newValue);

  char* valueFirst = _data.data() + (oldValueFirst - _data.data());
  const std::size_t oldHeaderValueSz = static_cast<std::size_t>(oldValueLast - oldValueFirst);

  const auto diff = static_cast<int64_t>(newValue.size()) - static_cast<int64_t>(oldHeaderValueSz);
  if (diff == 0) {
    if (!newValue.empty()) {
      Copy(newValue, valueFirst);
    }
    return;
  }

  const auto valuePos = static_cast<std::size_t>(valueFirst - _data.data());
  if (diff > 0) {
    _data.ensureAvailableCapacityExponential(static_cast<uint64_t>(diff));
    valueFirst = _data.data() + valuePos;
  }

  std::memmove(valueFirst + newValue.size(), valueFirst + oldHeaderValueSz, _data.size() - valuePos - oldHeaderValueSz);
  if (!newValue.empty()) {
    Copy(newValue, valueFirst);
  }

  _data.adjustSize(diff);

  adjustBodyStart(diff);
}

HttpResponse& HttpResponse::headerAppendValue(std::string_view key, std::string_view value, std::string_view sep) & {
  const auto [first, last] = HeadersLinearSearch(headersFlatView(), key);
  if (first == nullptr) {
    headerAddLine(key, value);
    return *this;
  }

  value = TrimOws(value);

  const std::size_t extraLen = sep.size() + value.size();
  if (extraLen == 0) {
    return *this;
  }

  const auto insertOffset = static_cast<std::size_t>(last - _data.data());
  const std::size_t tailLen = _data.size() - insertOffset;

  _data.ensureAvailableCapacityExponential(extraLen);

  char* insertPtr = _data.data() + insertOffset;

  std::memmove(insertPtr + extraLen, insertPtr, tailLen);

  char* out = insertPtr;
  if (!sep.empty()) {
    out = Append(sep, out);
  }
  if (!value.empty()) {
    Copy(value, out);
  }

  _data.addSize(extraLen);
  adjustBodyStart(static_cast<int64_t>(extraLen));

  return *this;
}

HttpResponse& HttpResponse::headerRemoveLine(std::string_view key) & {
  // We cannot remove Content-Type and Content-Length headers separately from the body,
  // so we don't include them in the search when response has body.
  const std::string_view flatHeaders = hasBody() ? headersFlatViewWithoutCTCL() : headersFlatView();
  const auto [first, last] = HeadersReverseLinearSearch(flatHeaders, key);
  if (first == nullptr) {
    return *this;
  }

  if (CaseInsensitiveEqual(key, http::ContentEncoding)) {
    if (hasBody()) {
      throw std::logic_error("Cannot remove Content-Encoding header when response has body");
    }
    _opts.setHasContentEncoding(false);
  }

  const std::size_t lineSize = HeaderSize(key.size(), static_cast<std::size_t>(last - first));
  const std::size_t sizeToMove = _data.size() - static_cast<std::size_t>(last + http::CRLF.size() - _data.data());
  char* dest = _data.data() + (first - _data.data()) - key.size() - http::HeaderSep.size();

  std::memmove(dest, last + http::CRLF.size(), sizeToMove);
  _data.setSize(_data.size() - lineSize);
  adjustBodyStart(static_cast<int64_t>(-lineSize));

  return *this;
}

HttpResponse& HttpResponse::headerRemoveValue(std::string_view key, std::string_view value, std::string_view sep) & {
  if (sep.empty()) [[unlikely]] {
    throw std::invalid_argument("Separator cannot be empty when removing a header value");
  }
  const auto [first, last] = HeadersReverseLinearSearch(headersFlatView(), key);
  if (first == nullptr) {
    return *this;
  }

  const std::string_view headerValue(first, last);
  if (value == headerValue) {
    // value matches the whole header value, we remove the whole header line
    const std::size_t lineSize = HeaderSize(key.size(), headerValue.size());
    const std::size_t sizeToMove = _data.size() - static_cast<std::size_t>(last + http::CRLF.size() - _data.data());
    char* dest = _data.data() + (first - _data.data()) - key.size() - http::HeaderSep.size();

    std::memmove(dest, last + http::CRLF.size(), sizeToMove);
    _data.setSize(_data.size() - lineSize);
    adjustBodyStart(static_cast<int64_t>(-lineSize));

    return *this;
  }

  const std::string_view occ = value.empty() ? sep : value;

  auto valueIt = first;
  while (true) {
    valueIt = std::search(valueIt, last, occ.begin(), occ.end());
    if (valueIt == last) {
      break;
    }

    // check if value is correctly surrounded by 'sep' (or at the beginning/end of the header value)
    if (valueIt != first && !value.empty() &&
        (first + sep.size() > valueIt || !std::equal(sep.begin(), sep.end(), valueIt - sep.size(), valueIt))) {
      // not preceded by separator, go to next occurrence (if any)
      valueIt += occ.size();
      continue;
    }

    if (valueIt + occ.size() != last && !value.empty() &&
        (last < valueIt + occ.size() + sep.size() ||
         !std::equal(sep.begin(), sep.end(), valueIt + occ.size(), valueIt + occ.size() + sep.size()))) {
      // not followed by separator, go to next occurrence (if any)
      valueIt += occ.size();
      continue;
    }

    // we found value to remove, we need to remove it along with the separator (if any)
    char* toRemoveFirst = _data.data() + (valueIt - _data.data());
    char* toRemoveLast = toRemoveFirst + occ.size();

    if (!value.empty()) {
      if (valueIt != first) {
        toRemoveFirst -= sep.size();
      } else {
        assert(valueIt + occ.size() != last);
        toRemoveLast += sep.size();
      }
    }

    std::memmove(toRemoveFirst, toRemoveLast, _data.size() - static_cast<std::size_t>(toRemoveLast - _data.data()));
    _data.setSize(_data.size() - static_cast<std::size_t>(toRemoveLast - toRemoveFirst));
    adjustBodyStart(-static_cast<int64_t>(toRemoveLast - toRemoveFirst));

    break;
  }

  return *this;
}

#ifdef AERONET_ENABLE_HTTP2
void HttpResponse::finalizeForHttp2() {
  // HTTP/2 requires lowercase header names. Do this after all response mutations (incl. compression).
  // TODO: optimize by building lowercase names directly when adding headers if response is "prepared"?
  // We could even go further in HTTP/2 - store all header names in lowercase and optimize header search by doing a case
  // sensitive search.
  char* first = _data.data() + headersStartPos() + kDateHeaderLenWithCRLF + http::CRLF.size();
  char* last = _data.data() + bodyStartPos() - http::CRLF.size();

  while (first < last) {
    for (; *first != ':'; ++first) {
      *first = tolower(*first);
    }

    // move headersBeg to next header
    first = std::search(first + http::HeaderSep.size(), last, http::CRLF.begin(), http::CRLF.end()) + http::CRLF.size();
  }

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  // If the response has trailers, the finalization of the body was made at the first added trailer in trailerAddLine.
  if (_opts.isAutomaticDirectCompression() && trailersSize() == 0) {
    finalizeInlineBody();
  }
#endif
}
#endif

[[nodiscard]] std::string_view HttpResponse::trailerValueOrEmpty(std::string_view key) const noexcept {
  const auto [first, last] = HeadersLinearSearch(trailersFlatView(), key);
  return {first, last};
}

HttpResponse& HttpResponse::trailerAddLine(std::string_view name, std::string_view value) & {
  assert(!http::IsForbiddenTrailerHeader(name));
  if (!http::IsValidHeaderName(name)) [[unlikely]] {
    throw std::invalid_argument("Invalid trailer header name");
  }
  if (!http::IsValidHeaderValue(value)) [[unlikely]] {
    throw std::invalid_argument("HTTP header value is invalid");
  }
  if (!hasBodyInMemory() && !_opts.isAutomaticDirectCompression()) [[unlikely]] {
    throw std::logic_error("Trailers must be added after a non empty body is set");
  }

  /*

  Example of a full HTTP/1.1 response with chunked transfer encoding and trailers:

  HTTP/1.1 200 OK\r\n
  Date: Tue, 12 Jan 2026 10:15:30 GMT\r\n
  Content-Type: text/plain\r\n
  Transfer-Encoding: chunked\r\n
  Trailer: Expires, Digest\r\n
  \r\n
  7\r\n
  Mozilla\r\n
  9\r\n
  Developer\r\n
  7\r\n
  Network\r\n
  0\r\n
  Expires: Wed, 13 Jan 2026 10:15:30 GMT\r\n
  Digest: SHA-256=47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=\r\n
  \r\n

  */

  // Important note - trailers are stored in the same payload as the body, at the end of it, not separated by a CRLF.
  // It is the responsibility of the finalization code to ensure that the body is in chunked format if trailers are
  // present.

  value = TrimOws(value);

  if (isHead()) {
    // If there are trailers in a HEAD response, we can keep content-length header as is, no need to convert to chunked
    // transfer-encoding. We add the Trailer header if the option is set. Indeed, if isHead is true here, then
    // necessarily the isAddTrailerHeader option is known as well.
    if (_opts.isAddTrailerHeader()) {
      headerAppendValue(http::Trailer, name, kTrailerValueSep);
    }
    return *this;
  }

  const std::size_t lineSize = HeaderSize(name.size(), value.size());
  const auto newTrailerLen = SafeCast<decltype(_opts._trailerLen)>(lineSize + trailersSize());

  // Optim - if we know that option is not set, no need to pre-reserve additional capacity for Trailer header
  const bool addTrailerHeader = !_opts.isPrepared() || _opts.isAddTrailerHeader();

  // Add an extra CRLF space for the last CRLF that will terminate trailers in finalize
  int64_t neededCapacity = static_cast<int64_t>(lineSize + http::CRLF.size());
  if (trailersSize() == 0) {
    if (addTrailerHeader) {
      neededCapacity += static_cast<int64_t>(HeaderSize(http::Trailer.size(), name.size()));
    }
    neededCapacity += TransferEncodingHeaderSizeDiff(nchars(bodyInMemoryLength()));

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    if (_opts.isAutomaticDirectCompression()) {
      finalizeInlineBody(neededCapacity);
    }
#endif
  } else if (addTrailerHeader) {
    neededCapacity += static_cast<int64_t>(name.size() + kTrailerValueSep.size());
  }

  char* insertPtr;
  if (hasBodyCaptured()) {
    assert(!_payloadVariant.isFilePayload());
    _payloadVariant.ensureAvailableCapacityExponential(neededCapacity);
    insertPtr = _payloadVariant.data() + _payloadVariant.size();
    _payloadVariant.addSize(lineSize);
  } else {
    _data.ensureAvailableCapacityExponential(neededCapacity);
    insertPtr = _data.data() + _data.size();
    _data.addSize(lineSize);
  }

  _opts._trailerLen = newTrailerLen;

  WriteHeaderCRLF(name, value, insertPtr);
  return *this;
}

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)

HttpResponse::Options::Options(internal::ResponseCompressionState& compressionState, Encoding expectedEncoding)
    : _pCompressionState(&compressionState),
      _pickedEncoding(expectedEncoding),
      _directCompressionMode(compressionState.pCompressionConfig->defaultDirectCompressionMode) {}

bool HttpResponse::Options::directCompressionPossible(std::size_t bodySize,
                                                      std::string_view contentType) const noexcept {
  if (!directCompressionPossible()) {
    return false;
  }
  if (_directCompressionMode == DirectCompressionMode::On) {
    return true;
  }
  const CompressionConfig& compressionConfig = *_pCompressionState->pCompressionConfig;
  if (bodySize < compressionConfig.minBytes) {
    return false;
  }

  assert(!contentType.empty());

  return compressionConfig.contentTypeAllowList.empty() ||
         compressionConfig.contentTypeAllowList.containsCI(contentType);
}

#endif

HttpResponseData HttpResponse::finalizeForHttp1(SysTimePoint tp, http::Version version, Options opts,
                                                const ConcatenatedHeaders* pGlobalHeaders,
                                                std::size_t minCapturedBodySize) {
  // Write the Http version (1.0 or 1.1)
  version.writeFull(_data.data());

  std::size_t bodySz = bodyLength();

  const std::string_view connectionValue = opts.isClose() ? http::close : http::keepalive;
  // HTTP/1.1 (RFC 7230 / RFC 9110) specifies that Connection: keep-alive is the default.
  // HTTP/1.0 is the opposite - Connection: close is the default.
  const bool addConnectionHeader =
      (opts.isClose() && version == http::HTTP_1_1) || (!opts.isClose() && version == http::HTTP_1_0);
  const bool hasHeaders = !headersFlatView().empty();
  const bool isHeadMethod = opts.isHeadMethod();
  const bool addTrailerHeader = trailersSize() != 0 && opts.isAddTrailerHeader();

  std::size_t totalNewHeadersSize =
      addConnectionHeader ? HeaderSize(http::Connection.size(), connectionValue.size()) : 0;

  std::bitset<HttpServerConfig::kMaxGlobalHeaders> globalHeadersToSkipBmp;

  bool writeAllGlobalHeaders = !_opts.isPrepared();
  if (writeAllGlobalHeaders) {
    std::size_t pos = 0;
    assert(pGlobalHeaders != nullptr);
    for (std::string_view headerKeyVal : *pGlobalHeaders) {
      std::string_view key = headerKeyVal.substr(0, headerKeyVal.find(':'));
      if (hasHeaders && headerValue(key)) {
        // Header already present, skip it
        globalHeadersToSkipBmp.set(pos);
        writeAllGlobalHeaders = false;
      } else {
        totalNewHeadersSize += headerKeyVal.size() + http::CRLF.size();
      }

      ++pos;
    }
  }

  assert(totalNewHeadersSize + bodyStartPos() <= kBodyStartMask);

  char* headersInsertPtr = nullptr;

  if ((bodySz == 0
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
       && !_opts.isAutomaticDirectCompression()
#endif
           ) ||
      isHeadMethod || hasBodyFile()) {
    // For HEAD responses we must not transmit the body, but keep file payloads
    // intact so ownership can be transferred by finalizeForHttp1. For inline
    // bodies we still erase the inline bytes.
    if (isHeadMethod && !hasBodyFile()) {
      if (trailersSize() != 0 && addTrailerHeader) {
        // Strategy: we will override Content-Length header with Trailer header, so that we know we will not override
        // trailers themselves during the append. At the end, we will append the Content-Length header back.
        std::size_t trailerHeaderValueSize = 0;
        std::string_view flatTrailersView = trailersFlatView();
        for (const auto& [name, value] : HeadersView(flatTrailersView)) {
          trailerHeaderValueSize += name.size() + kTrailerValueSep.size();
        }
        assert(trailerHeaderValueSize != 0);
        trailerHeaderValueSize -= kTrailerValueSep.size();

        const auto neededCapacity = totalNewHeadersSize + HeaderSize(http::Trailer.size(), trailerHeaderValueSize);
        _data.ensureAvailableCapacity(neededCapacity);

        if (hasBodyInlined()) {
          flatTrailersView = internalTrailers();  // may have been reallocated
          char* newTrailersDataPtr = _data.data() + (flatTrailersView.data() - _data.data()) + neededCapacity;
          std::memmove(newTrailersDataPtr, flatTrailersView.data(), flatTrailersView.size());
          flatTrailersView = std::string_view(newTrailersDataPtr, flatTrailersView.size());
        }

        const auto bodySzStrVec = IntegralToCharVector(bodySz);

        char* insertPtr = Append(http::Trailer, getContentLengthHeaderLinePtr() + http::CRLF.size());
        insertPtr = Append(http::HeaderSep, insertPtr);
        bool isFirst = true;
        for (const auto& [name, value] : HeadersView(flatTrailersView)) {
          if (!isFirst) {
            insertPtr = Append(kTrailerValueSep, insertPtr);
          }
          insertPtr = Append(name, insertPtr);
          isFirst = false;
        }
        insertPtr = Append(http::CRLF, insertPtr);
        // Now append back Content-Length header
        insertPtr = Append(http::ContentLength, insertPtr);
        insertPtr = Append(http::HeaderSep, insertPtr);

        insertPtr = Append(std::string_view(bodySzStrVec), insertPtr);
        insertPtr = Append(http::DoubleCRLF, insertPtr);
        const auto newBodyStartPos = static_cast<std::size_t>(insertPtr - _data.data());
        _data.setSize(newBodyStartPos);
        setBodyStartPos(newBodyStartPos);
      } else if (hasBodyInlined()) {
        _data.setSize(_data.size() - bodySz - trailersSize());
      }
      _opts._trailerLen = 0;
      _payloadVariant = {};
    }

    if (totalNewHeadersSize != 0) {
      _data.ensureAvailableCapacity(totalNewHeadersSize);
      headersInsertPtr = _data.data() + bodyStartPos() - http::DoubleCRLF.size();
      // Copy \r\n\r\n to its final place
      Copy(http::DoubleCRLF, headersInsertPtr + totalNewHeadersSize);
    }
  } else {
    // body > 0 && !isHeadMethod && !hasFileBody()
    bool moveBodyInline = hasBodyCaptured() && bodySz + trailersSize() <= minCapturedBodySize;
    if (trailersSize() == 0) {
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
      if (_opts.isAutomaticDirectCompression()) {
        finalizeInlineBody(static_cast<int64_t>(totalNewHeadersSize));
        bodySz = bodyInlinedSize();
      } else {
#endif
        _data.ensureAvailableCapacity((moveBodyInline ? bodySz : 0) + totalNewHeadersSize);
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
      }
#endif
      char* oldBodyStart = _data.data() + bodyStartPos();
      headersInsertPtr = oldBodyStart - http::DoubleCRLF.size();
      if (moveBodyInline) {
        if (totalNewHeadersSize != 0) {
          // Copy \r\n\r\n to its final place
          Copy(http::DoubleCRLF, headersInsertPtr + totalNewHeadersSize);
        }
        const auto bodyAndTrailersView = _payloadVariant.view();
        Copy(bodyAndTrailersView, oldBodyStart + totalNewHeadersSize);

        _data.addSize(bodySz);

        _payloadVariant = {};
      } else if (totalNewHeadersSize != 0) {
        if (!hasBodyCaptured()) {
          // Move inline body to its final place
          std::memmove(oldBodyStart + totalNewHeadersSize, oldBodyStart, bodySz);
        }

        // Copy \r\n\r\n to its final place
        Copy(http::DoubleCRLF, headersInsertPtr + totalNewHeadersSize);
      }
    } else {
      // RFC 7230 §4.1.2: Trailers require chunked transfer encoding.
      // Convert response to chunked format before proceeding with finalization.
      // Note: HEAD responses don't transmit body/trailers.

      // Calculate size difference between Content-Length header and Transfer-Encoding: chunked header
      // Content-Length: N -> Transfer-Encoding: chunked
      // Old header size: CRLF + "content-length" + ": " + nchars(bodySz)
      // New header size: CRLF + "transfer-encoding" + ": " + "chunked"
      const auto nCharsBodyLen = nchars(bodySz);
      int64_t headerSizeDiff = TransferEncodingHeaderSizeDiff(nCharsBodyLen);

      if (addTrailerHeader) {
        // We need to add the Trailer header as well
        std::size_t trailerHeaderValueSz = 0;

        for (const auto& [name, value] : trailers()) {
          trailerHeaderValueSz += name.size() + kTrailerValueSep.size();
        }
        assert(trailerHeaderValueSz != 0);
        trailerHeaderValueSz -= kTrailerValueSep.size();

        headerSizeDiff += static_cast<int64_t>(HeaderSize(http::Trailer.size(), trailerHeaderValueSz));
      }

      // Calculate extra space needed for chunked framing:
      // Before body: hex(len)\r\n
      // After body (before trailers): \r\n0\r\n
      const auto hexDigits = hex_digits(bodySz);

      if (hasBodyCaptured()) {
        // External payload case: body is in captured buffer, trailers are also there
        // We need to:
        // 1. Replace Content-Length header with Transfer-Encoding: chunked in _data
        // 2. Wrap external body with chunked framing

        std::size_t neededSize = http::CRLF.size() + hexDigits + static_cast<std::size_t>(headerSizeDiff);
        if (moveBodyInline) {
          neededSize += _payloadVariant.size() + kEndChunkedBody.size() + http::CRLF.size();
        }

        _data.ensureAvailableCapacity(neededSize + totalNewHeadersSize);

        char* newDoubleCRLFPtr = _data.data() + bodyStartPos() - http::DoubleCRLF.size() + totalNewHeadersSize +
                                 static_cast<std::size_t>(headerSizeDiff);

        // Now update headers in _data: replace Content-Length with Transfer-Encoding: chunked
        // Find and replace Content-Length header
        headersInsertPtr = ReplaceContentLengthWithTransferEncoding(getContentLengthHeaderLinePtr(), trailersFlatView(),
                                                                    addTrailerHeader);

        char* insertPtr = to_lower_hex(bodySz, headersInsertPtr + totalNewHeadersSize + http::DoubleCRLF.size());
        insertPtr = Append(http::CRLF, insertPtr);

        Copy(http::DoubleCRLF, newDoubleCRLFPtr);

        if (moveBodyInline) {
          auto bodyAndTrailersView = _payloadVariant.view();

          // Body only without trailers
          std::memcpy(insertPtr, bodyAndTrailersView.data(), bodySz);
          insertPtr += bodySz;

          // Write end chunked body
          insertPtr = Append(kEndChunkedBody, insertPtr);

          // trailers
          std::memcpy(insertPtr, bodyAndTrailersView.data() + bodySz, trailersSize());
          insertPtr += trailersSize();

          // Final CRLF
          Copy(http::CRLF, insertPtr);

          _payloadVariant = {};
        } else {
          // Now, the rest will be done in the external payload. The body does not move, only the chunk
          // headers/footers are added.
          _payloadVariant.ensureAvailableCapacity(kEndChunkedBody.size() + http::CRLF.size());
          _payloadVariant.insert(bodySz, kEndChunkedBody);
          _payloadVariant.append(http::CRLF);
        }
        _data.addSize(neededSize);
      } else {
        // Inline body case: body and trailers are in _data

        // Calculate new total size (including final CRLF after trailers)

        // For up to 64 bits size (actually, 375 bits, see below), diffSz cannot be negative.
        // In fact, to be negative, the size must be so large that the decimal representation size minus the hex
        // representation size is greater than constantDiff. This happens for sizes that are larger than 2^375 (approx
        // 10^113), which is way beyond any practical HTTP body size.
        // with constantDiff = len("transfer-encoding") - len("content-length") + len(endChunkedBody) + len(\r\n\r\n)
        //                   = 19
        static constexpr std::size_t kMaxTheoreticalBodySize = std::numeric_limits<std::size_t>::max();
        static constexpr int64_t kMinHeaderSizeDiff = TransferEncodingHeaderSizeDiff(nchars(kMaxTheoreticalBodySize));
        static constexpr std::size_t kMaxHexDigits = hex_digits(kMaxTheoreticalBodySize);
        static constexpr int64_t kMinAddedSizeForChunked =
            static_cast<int64_t>(kMaxHexDigits + kEndChunkedBody.size() + http::DoubleCRLF.size());

        static_assert(kMinHeaderSizeDiff + kMinAddedSizeForChunked >= 0, "Need to care for overflows otherwise");

        std::size_t diffSz =
            hexDigits + kEndChunkedBody.size() + http::DoubleCRLF.size() + static_cast<std::size_t>(headerSizeDiff);

        std::size_t neededSize = diffSz + totalNewHeadersSize;
        _data.ensureAvailableCapacity(neededSize);

        const auto oldBodyStart = bodyStartPos();
        const auto oldTrailerStart = oldBodyStart + bodySz;

        // Strategy: work from back to front to avoid overwriting data we need
        // Final layout: [headers with
        // TE:chunked][DoubleCRLF][chunk-header][body][chunk-end][last-chunk][trailers][CRLF]

        const auto newHexStart = oldBodyStart + totalNewHeadersSize + static_cast<std::size_t>(headerSizeDiff);

        // Calculate positions in new layout
        const auto newBodyDataStart = newHexStart + hexDigits + http::CRLF.size();
        const auto newLastChunkStart = newBodyDataStart + bodySz + http::CRLF.size();
        const auto newTrailersStart = newLastChunkStart + 1UL + http::CRLF.size();  // "0\r\n"
        std::string_view newTrailersFlatView(_data.data() + newTrailersStart, static_cast<std::size_t>(trailersSize()));

        // Write final CRLF first (it's at the very end)
        Copy(http::CRLF, _data.data() + newTrailersStart + trailersSize());

        // Move trailers (they go before final CRLF) - use offset, not old view
        std::memmove(_data.data() + newTrailersStart, _data.data() + oldTrailerStart, trailersSize());

        // Write last-chunk "\r\n0\r\n"
        Copy(kEndChunkedBody, _data.data() + newLastChunkStart - http::CRLF.size());

        // Move body data - use offset, not old view
        std::memmove(_data.data() + newBodyDataStart, _data.data() + oldBodyStart, bodySz);

        // Write chunk header "hex(len)\r\n"
        Copy(http::CRLF, to_lower_hex(bodySz, _data.data() + newHexStart));

        // Move and update headers: replace Content-Length with Transfer-Encoding: chunked
        // Move everything before body (headers and DoubleCRLF)
        // Find Content-Length header position (relative to start)
        headersInsertPtr = ReplaceContentLengthWithTransferEncoding(getContentLengthHeaderLinePtr(),
                                                                    newTrailersFlatView, addTrailerHeader);

        Copy(http::DoubleCRLF, headersInsertPtr + totalNewHeadersSize);

        // Update buffer size and body start position
        _data.addSize(diffSz);
      }
    }
  }

  if (!_opts.isPrepared() && !pGlobalHeaders->empty()) {
    if (writeAllGlobalHeaders) {
      // Optim: single copy for all global headers
      headersInsertPtr = Append(http::CRLF, headersInsertPtr);
      headersInsertPtr = Append(pGlobalHeaders->fullString(), headersInsertPtr);
    } else {
      std::size_t pos = 0;
      for (std::string_view headerKeyVal : *pGlobalHeaders) {
        if (!globalHeadersToSkipBmp.test(pos)) {
          headersInsertPtr = Append(http::CRLF, headersInsertPtr);
          headersInsertPtr = Append(headerKeyVal, headersInsertPtr);
        }
        ++pos;
      }
    }
  }

  if (addConnectionHeader) {
    headersInsertPtr = WriteCRLFHeader(http::Connection, connectionValue, headersInsertPtr);
  }

  _data.addSize(totalNewHeadersSize);

  WriteCRLFDateHeader(tp, _data.data() + headersStartPos());

  assert(!_payloadVariant.isSizeOnly());

  HttpResponseData prepared(std::move(_data), std::move(_payloadVariant));

  if (isHeadMethod) {
    auto* pFilePayload = prepared.getIfFilePayload();
    if (pFilePayload != nullptr) {
      pFilePayload->length = 0;
    }
  }

  return prepared;
}

void HttpResponse::bodyAppendUpdateHeaders(std::string_view givenContentType, std::string_view defaultContentType,
                                           std::size_t totalBodyLen) {
  assert(trailersSize() == 0);
  if (bodyLength() == 0 && !_opts.isAutomaticDirectCompression()) {
    if (givenContentType.empty()) {
      givenContentType = defaultContentType;
    }
    addContentTypeAndContentLengthHeaders(givenContentType, totalBodyLen);
  } else {
    if (!givenContentType.empty()) {
      replaceHeaderValueNoRealloc(getContentTypeValuePtr(), givenContentType);
    }
    replaceHeaderValueNoRealloc(getContentLengthValuePtr(), std::string_view(IntegralToCharVector(totalBodyLen)));
  }
}

void HttpResponse::replaceHeaderValueNoRealloc(char* first, std::string_view newValue) {
  char* last = first;
  while (*last != '\r') {
    ++last;
  }
  const auto oldValueLen = static_cast<std::size_t>(last - first);
  if (newValue.size() != oldValueLen) {
    const auto diff = static_cast<int64_t>(newValue.size()) - static_cast<int64_t>(oldValueLen);
    std::memmove(last + diff, last, static_cast<std::size_t>(_data.end() - last));
    _data.adjustSize(diff);
    adjustBodyStart(diff);
  }
  // This function is only called to set reserved headers Content-Length (which is never empty) and Content-Type
  // (which we never set to empty, it would be rejected by a std::invalid_argument).
  Copy(newValue, first);
}

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)

std::size_t HttpResponse::appendEncodedInlineOrThrow(std::string_view data) {
  assert(_opts._pCompressionState != nullptr);
  auto& compressionState = *_opts._pCompressionState;
  auto& encoderCtx = *compressionState.context(_opts._pickedEncoding);
  assert(encoderCtx.minEncodeChunkCapacity(data.size()) <= _data.availableCapacity());
  const auto result = encoderCtx.encodeChunk(data, _data.availableCapacity(), _data.data() + _data.size());

  if (result.hasError()) [[unlikely]] {
    // this cannot happen for lack of space, so it would indicate a real underlying issue (lack of memory, or other
    // encoder error)
    throw std::runtime_error("HttpResponse::appendEncodedInlineOrThrow compression failed");
  }

  return result.written();
}

void HttpResponse::finalizeInlineBody(int64_t additionalCapacity) {
  assert(_opts.isAutomaticDirectCompression() && !isHead() && !hasBodyCaptured() && !hasBodyFile());
  std::size_t oldBodyLen = internalBodyAndTrailersLen();
  auto& encoder = *_opts._pCompressionState->context(_opts._pickedEncoding);
  const std::size_t chunkSize = encoder.endChunkSize();
  while (true) {
    const auto nbCharsOldBodyLen = nchars(oldBodyLen);
    const auto nbCharsNewBodyLen = nchars(oldBodyLen + chunkSize);

    const int64_t neededCapacity =
        additionalCapacity + static_cast<int64_t>(chunkSize + nbCharsNewBodyLen - nbCharsOldBodyLen);
    _data.ensureAvailableCapacityExponential(neededCapacity);

    const auto result = encoder.end(_data.availableCapacity(), _data.data() + _data.size());
    if (result.hasError()) [[unlikely]] {
      throw std::runtime_error("Failed to finalize compressed response body");
    }

    const auto written = result.written();
    if (written == 0) {
      break;
    }

    _data.addSize(written);
    oldBodyLen += written;

    const auto newBodyLenCharVec = IntegralToCharVector(oldBodyLen);
    // TODO: avoid memmove of 'large' bodies if the number of chars of the body length changes (e.g. from 999 to 1000
    // bytes) by playing on 'spaces' after the content-length value.
    replaceHeaderValueNoRealloc(getContentLengthValuePtr(), std::string_view(newBodyLenCharVec));
  }
}

#endif

void HttpResponse::removeBodyAndItsHeaders() {
  // Remove all body, Content-Length and Content-Encoding headers at once.
  char* contentTypeHeaderLinePtr = getContentTypeHeaderLinePtr();
  assert(std::string_view(contentTypeHeaderLinePtr + http::CRLF.size(), http::ContentType.size()) == http::ContentType);
  _data.setSize(static_cast<std::size_t>(contentTypeHeaderLinePtr - _data.data()) + http::DoubleCRLF.size());
  Copy(http::CRLF, _data.data() + _data.size() - http::CRLF.size());
  setBodyStartPos(_data.size());

  // Also remove vary and content-encoding headers if present
  if (hasContentEncoding()) {
    headerRemoveLine(http::ContentEncoding);
    assert(!hasContentEncoding());
    _opts.setAutomaticDirectCompression(false);
    if (_opts.isAddVaryAcceptEncoding()) {
      headerRemoveValue(http::Vary, http::AcceptEncoding);
    }
  }
}

}  // namespace aeronet
