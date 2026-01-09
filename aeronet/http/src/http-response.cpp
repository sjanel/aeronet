#include "aeronet/http-response.hpp"

#include <algorithm>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/file.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"
#include "aeronet/toupperlower.hpp"

#ifndef NDEBUG
#include "aeronet/reserved-headers.hpp"
#endif

namespace aeronet {

namespace {
// The RFC does not specify a maximum length for the reason phrase,
// but in practice it should be reasonable. If you need a longer reason,
// consider using headers / body instead.
constexpr std::string_view::size_type kMaxReasonLength = 1024;

// Number of digits in the status code (3 digits).
constexpr std::size_t kNdigitsStatusCode = 3U;

// Date header will always be present at headersStartPos.
constexpr std::size_t kDateHeaderLenWithCRLF = HttpResponse::HeaderSize(http::Date.size(), kRFC7231DateStrLen);

constexpr std::size_t kStatusLineMinLenWithoutCRLF = http::HTTP10Sv.size() + 1U + kNdigitsStatusCode;

constexpr std::size_t kHttpResponseInitialSize =
    kStatusLineMinLenWithoutCRLF + kDateHeaderLenWithCRLF + http::DoubleCRLF.size();

static_assert(kHttpResponseInitialSize <= HttpResponse::kHttpResponseMinInitialCapacity,
              "Initial size should be less than or equal to min initial capacity");

constexpr std::size_t HttpResponseInitialSize(std::size_t reasonLen) {
  return kHttpResponseInitialSize + (reasonLen == 0 ? 0UL : std::min(reasonLen, kMaxReasonLength) + 1UL);
}

constexpr std::string_view AdjustReasonLen(std::string_view reason) {
  if (reason.size() > kMaxReasonLength) [[unlikely]] {
    log::warn("Provided reason is too long ({} bytes), truncating it to {} bytes", reason.length(), kMaxReasonLength);
    reason.remove_suffix(reason.size() - kMaxReasonLength);
  }
  return reason;
}
}  // namespace

HttpResponse::HttpResponse(http::StatusCode code, std::string_view reason)
    : HttpResponse(kHttpResponseMinInitialCapacity, code, reason) {}

HttpResponse::HttpResponse(std::size_t expectedUserCapacity, http::StatusCode code)
    : _data(kHttpResponseInitialSize + expectedUserCapacity) {
  _data[http::HTTP10Sv.size()] = ' ';
  static constexpr auto bodyStart = kHttpResponseInitialSize;
  _data[kReasonBeg] = '\n';
  setHeadersStartPos(static_cast<std::uint16_t>(kStatusLineMinLenWithoutCRLF));
  setBodyStartPos(bodyStart);
  status(code);
  std::memcpy(_data.data() + bodyStart - http::DoubleCRLF.size(), http::DoubleCRLF.data(), http::DoubleCRLF.size());
  _data.setSize(bodyStart);
}

HttpResponse::HttpResponse(std::size_t expectedUserCapacity, http::StatusCode code, std::string_view reason)
    : _data(HttpResponseInitialSize(reason.size()) + expectedUserCapacity) {
  _data[http::HTTP10Sv.size()] = ' ';
  const auto bodyStart = HttpResponseInitialSize(reason.size());
  setHeadersStartPos(static_cast<std::uint16_t>(bodyStart - http::DoubleCRLF.size() - kDateHeaderLenWithCRLF));
  setBodyStartPos(bodyStart);
  status(code);
  if (reason.empty()) {
    _data[kReasonBeg] = '\n';
  } else {
    reason = AdjustReasonLen(reason);
    _data[kReasonBeg - 1UL] = ' ';
    std::memcpy(_data.data() + kReasonBeg, reason.data(), reason.size());
  }
  std::memcpy(_data.data() + bodyStart - http::DoubleCRLF.size(), http::DoubleCRLF.data(), http::DoubleCRLF.size());
  _data.setSize(bodyStart);
}

HttpResponse::HttpResponse(std::string_view body, std::string_view contentType)
    : HttpResponse(BodySize(body.size(), contentType.size()), http::StatusCodeOK) {
  this->body(body, contentType);
}

std::size_t HttpResponse::bodyLen() const noexcept {
  if (const FilePayload* pFilePayload = filePayloadPtr(); pFilePayload != nullptr) {
    return static_cast<std::size_t>(pFilePayload->length);
  }
  const HttpPayload* pExternPayload = externPayloadPtr();
  return (pExternPayload != nullptr ? pExternPayload->size() : internalBodyAndTrailersLen()) - _trailerLen;
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
  auto oldReasonSz = reasonLen();
  int32_t diff = static_cast<int32_t>(newReason.size()) - static_cast<int32_t>(oldReasonSz);
  if (diff == 0) {
    if (!newReason.empty()) {
      std::memcpy(_data.data() + kReasonBeg, newReason.data(), newReason.size());
    }
    return *this;
  }
  if (diff > 0) {
    if (oldReasonSz == 0) {
      ++diff;  // for the space before first reason char
    }
    _data.ensureAvailableCapacityExponential(static_cast<std::size_t>(diff));
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
      _data.size() - kStatusCodeBeg - kNdigitsStatusCode - oldReasonSz - static_cast<uint32_t>(!oldReason.empty()));
  adjustBodyStart(diff);
  adjustHeadersStart(diff);
  if (newReason.empty()) {
    _data[kReasonBeg] = '\n';  // needed marker for empty reason
  } else {
    _data[kReasonBeg - 1UL] = ' ';
    std::memcpy(_data.data() + kReasonBeg, newReason.data(), newReason.size());
  }
  _data.addSize(static_cast<std::size_t>(diff));
  return *this;
}

void HttpResponse::setHeader(std::string_view newKey, std::string_view newValue, OnlyIfNew onlyIfNew) {
  assert(http::IsValidHeaderName(newKey));

  auto optValue = headerValue(newKey);
  if (!optValue) {
    headerAddLine(newKey, newValue);
    return;
  }
  if (onlyIfNew == OnlyIfNew::Yes) {
    return;
  }

  char* valueFirst = _data.data() + (optValue->data() - _data.data());
  const std::size_t oldHeaderValueSz = optValue->size();

  const auto diff = static_cast<int64_t>(newValue.size()) - static_cast<int64_t>(oldHeaderValueSz);
  if (diff == 0) {
    std::memcpy(valueFirst, newValue.data(), newValue.size());
    return;
  }

  const auto valuePos = static_cast<std::size_t>(valueFirst - _data.data());
  if (diff > 0) {
    _data.ensureAvailableCapacityExponential(static_cast<std::size_t>(diff));
    valueFirst = _data.data() + valuePos;
  }

  std::memmove(valueFirst + newValue.size(), valueFirst + oldHeaderValueSz, _data.size() - valuePos - oldHeaderValueSz);
  std::memcpy(valueFirst, newValue.data(), newValue.size());

  // Works even if diff is negative, the unsigned value will overflow and have a resulting value of exactly sz - diff.
  // In C++, unsigned overflow is well-defined.
  _data.addSize(static_cast<std::size_t>(diff));

  adjustBodyStart(diff);
}

namespace {
inline void SetBodyEnsureNoTrailers(std::size_t trailerLen) {
  if (trailerLen != 0) [[unlikely]] {
    throw std::logic_error("Cannot set body after the first trailer");
  }
}

}  // namespace

void HttpResponse::setBodyHeaders(std::string_view contentTypeValue, std::size_t newBodySize,
                                  bool setContentTypeIfPresent) {
  SetBodyEnsureNoTrailers(_trailerLen);

  const auto newBodyLenCharVec = IntegralToCharVector(newBodySize);
  const auto oldBodyLen = bodyLen();
  if (newBodySize == 0) {
    if (oldBodyLen != 0) {
      char* contentTypeHeaderLinePtr = getContentTypeHeaderLinePtr(oldBodyLen);
      assert(std::string_view(contentTypeHeaderLinePtr + http::CRLF.size(), http::ContentType.size()) ==
             http::ContentType);
      _data.setSize(static_cast<std::size_t>(contentTypeHeaderLinePtr - _data.data()) + http::CRLF.size());
      _data.unchecked_append(http::CRLF);
      setBodyStartPos(_data.size());
    }
  } else {
    const auto newContentTypeHeaderSize = HeaderSize(http::ContentType.size(), contentTypeValue.size());
    const auto newContentLengthHeaderSize = HeaderSize(http::ContentLength.size(), newBodyLenCharVec.size());
    const auto neededNewSize = newContentTypeHeaderSize + newContentLengthHeaderSize + newBodySize;
    if (oldBodyLen == 0) {
      _data.ensureAvailableCapacityExponential(neededNewSize);
      headerAddLine(http::ContentType, contentTypeValue);
      headerAddLine(http::ContentLength, std::string_view(newBodyLenCharVec));
    } else {
      const auto oldContentTypeAndLengthSize =
          bodyStartPos() - static_cast<std::size_t>(getContentTypeHeaderLinePtr(oldBodyLen) - _data.data()) -
          http::DoubleCRLF.size();

      if (neededNewSize > oldContentTypeAndLengthSize + oldBodyLen) {
        _data.ensureAvailableCapacityExponential(neededNewSize - oldContentTypeAndLengthSize - oldBodyLen);
      }
      if (setContentTypeIfPresent) {
        replaceHeaderValueNoRealloc(getContentTypeValuePtr(oldBodyLen), contentTypeValue);
      }
      replaceHeaderValueNoRealloc(getContentLengthValuePtr(oldBodyLen), std::string_view(newBodyLenCharVec));
    }
  }
}

void HttpResponse::setBodyInternal(std::string_view newBody) {
  const int64_t diff = static_cast<int64_t>(newBody.size()) - static_cast<int64_t>(internalBodyAndTrailersLen());
  if (diff > 0) {
    int64_t newBodyInternalPos = -1;
    if (newBody.data() > _data.data() && newBody.data() <= _data.data() + _data.size()) {
      // the memory pointed by newBody is internal to HttpResponse. We need to save the position before realloc
      newBodyInternalPos = newBody.data() - _data.data();
    }
    _data.ensureAvailableCapacityExponential(static_cast<std::size_t>(diff));
    if (newBodyInternalPos != -1) {
      // restore the original data
      newBody = std::string_view(_data.data() + newBodyInternalPos, newBody.size());
    }
    _data.addSize(static_cast<std::size_t>(diff));
  } else {
    _data.setSize(_data.size() - static_cast<std::size_t>(-diff));
  }
  if (!newBody.empty()) {
    // Because calling memcpy with a null pointer is undefined behavior even if size is 0
    std::memcpy(_data.data() + bodyStartPos(), newBody.data(), newBody.size());
  }
  // Clear payload variant at the end because newBody may point to its internal memory.
  _payloadVariant = {};
}

HttpResponse& HttpResponse::bodyAppend(std::string_view body, std::string_view contentType) & {
  if (!body.empty()) {
    HttpPayload* pExternPayload = externPayloadPtr();
    if (pExternPayload == nullptr) {
      if (hasFileBody()) [[unlikely]] {
        throw std::logic_error("Cannot append to a captured file body");
      }
      const bool setContentTypeIfPresent = !contentType.empty();

      if (contentType.empty()) {
        contentType = http::ContentTypeTextPlain;
      }
      setBodyHeaders(contentType, internalBodyAndTrailersLen() + body.size(), setContentTypeIfPresent);

      _data.unchecked_append(body);
    } else {
      if (!contentType.empty()) {
        setHeader(http::ContentType, contentType);
      }
      pExternPayload->append(body);
    }
  }
  return *this;
}

HttpResponse& HttpResponse::file(File fileObj, std::size_t offset, std::size_t length, std::string_view contentType) & {
  if (!fileObj) {
    throw std::invalid_argument("file requires an opened file");
  }
  if (_trailerLen != 0) [[unlikely]] {
    throw std::logic_error("Cannot set body after the first trailer");
  }
  const std::size_t fileSize = fileObj.size();
  if (fileSize == File::kError) {
    throw std::invalid_argument("file size is unknown");
  }
  if (fileSize < offset) {
    throw std::invalid_argument("file offset exceeds file size");
  }
  const std::size_t resolvedLength = length == 0 ? (fileSize - offset) : length;
  if (fileSize < offset + resolvedLength) {
    throw std::invalid_argument("file length exceeds file size");
  }
  if (contentType.empty()) {
    setHeader(http::ContentType, fileObj.detectedContentType());
  } else {
    setHeader(http::ContentType, contentType);
  }
  // If file is empty, we emit on purpose Content-Length: 0 and no body.
  // This is to distinguish an empty file response from a response with no body at all.
  // This is valid per HTTP semantics.
  setHeader(http::ContentLength, std::string_view(IntegralToCharVector(resolvedLength)));

  setBodyInternal(std::string_view());
  _payloadVariant.emplace<FilePayload>(std::move(fileObj), offset, resolvedLength);
  return *this;
}

const File* HttpResponse::file() const noexcept {
  const auto* pFilePayload = std::get_if<FilePayload>(&_payloadVariant);
  return pFilePayload == nullptr ? nullptr : &pFilePayload->file;
}

std::string_view HttpResponse::body() const noexcept {
  const HttpPayload* pExternPayload = externPayloadPtr();
  auto ret = pExternPayload != nullptr ? pExternPayload->view()
                                       : std::string_view{_data.begin() + bodyStartPos(), _data.end()};
  ret.remove_suffix(_trailerLen);
  return ret;
}

namespace {
// Performs a linear search for the header 'key' in the headers block defined by flatHeaders.
// flatHeaders should start at the start of the first header, without any leading CRLF.
// flatHeaders should end immediately after the last header CRLF.
// Returns std::nullopt if the header is not found, or the value as a std::string_view otherwise.
constexpr std::optional<std::string_view> HeadersLinearSearch(std::string_view flatHeaders,
                                                              std::string_view key) noexcept {
  std::optional<std::string_view> ret;
  const char* endKey = key.end();
  const char* headersBeg = flatHeaders.data();
  const char* headersEnd = headersBeg + flatHeaders.size();

  while (headersBeg < headersEnd) {
    // Perform an inplace case-insensitive 'starts_with' algorithm
    const char* begKey = key.begin();
    bool icharsDiffer = false;
    for (; *headersBeg != ':' && begKey != endKey; ++headersBeg, ++begKey) {
      if (tolower(*headersBeg) != tolower(*begKey)) {
        icharsDiffer = true;
        break;
      }
    }

    const char* nextCRLF =
        std::search(headersBeg + http::HeaderSep.size(), headersEnd, http::CRLF.begin(), http::CRLF.end());

    if (!icharsDiffer && *headersBeg == ':' && begKey == endKey) {
      // Found the header we are looking for
      ret = {headersBeg + http::CRLF.size(), nextCRLF};
      break;
    }

    // Not the header we are looking for - move headersBeg to next header
    headersBeg = nextCRLF + http::CRLF.size();
  }
  return ret;
}

}  // namespace

std::optional<std::string_view> HttpResponse::trailerValue(std::string_view key) const noexcept {
  return HeadersLinearSearch(trailersFlatView(), key);
}

std::string_view HttpResponse::headersFlatView() const noexcept {
  return {_data.data() + headersStartPos() + kDateHeaderLenWithCRLF + http::CRLF.size(),
          _data.data() + bodyStartPos() - http::CRLF.size()};
}

std::string_view HttpResponse::headersFlatViewWithDate() const noexcept {
  return {_data.data() + headersStartPos() + http::CRLF.size(), _data.data() + bodyStartPos() - http::CRLF.size()};
}

std::optional<std::string_view> HttpResponse::headerValue(std::string_view key) const noexcept {
  // Start the search after reserved headers unconditionally added at response creation
  return HeadersLinearSearch(headersFlatView(), key);
}

HttpResponse& HttpResponse::headerAddLine(std::string_view key, std::string_view value) & {
  assert(http::IsValidHeaderName(key) && !CaseInsensitiveEqual(key, http::Date));

  const std::size_t headerLineSize = HeaderSize(key.size(), value.size());

  _data.ensureAvailableCapacityExponential(headerLineSize);

  char* insertPtr = _data.data() + bodyStartPos() - http::DoubleCRLF.size();

  const auto bodySz = bodyLen();

  if (bodySz == 0) {
    std::memmove(insertPtr + headerLineSize, insertPtr, http::DoubleCRLF.size());
  } else {
    // We want to keep Content-Type and Content-Length together with the body (we use this property for optimization)
    // so we insert new headers before them. Of course, this code takes time, but it should be rare to add headers
    // after setting the body, so we can consider this as a 'slow' path.
    // Find the position of the Content-Type header going backwards from body start.
    insertPtr = getContentTypeHeaderLinePtr(bodySz);
    std::memmove(insertPtr + headerLineSize, insertPtr, static_cast<std::size_t>(_data.end() - insertPtr));
  }
  WriteCRLFHeader(insertPtr, key, value);
  _data.addSize(headerLineSize);

  adjustBodyStart(static_cast<int64_t>(headerLineSize));

  return *this;
}

HttpResponse& HttpResponse::headerAppendValue(std::string_view key, std::string_view value,
                                              std::string_view separator) & {
  assert(!http::IsReservedResponseHeader(key));
  assert(http::IsValidHeaderName(key));

  auto optValue = headerValue(key);
  if (!optValue) {
    headerAddLine(key, value);
    return *this;
  }

  const std::size_t extraLen = separator.size() + value.size();
  if (extraLen == 0) {
    return *this;
  }

  const auto valuePos = static_cast<std::size_t>(optValue->data() - _data.data());
  const auto insertOffset = valuePos + optValue->size();
  const std::size_t tailLen = _data.size() - insertOffset;

  _data.ensureAvailableCapacityExponential(extraLen);

  char* insertPtr = _data.data() + insertOffset;

  std::memmove(insertPtr + extraLen, insertPtr, tailLen);

  char* out = insertPtr;
  if (!separator.empty()) {
    std::memcpy(out, separator.data(), separator.size());
    out += separator.size();
  }
  if (!value.empty()) {
    std::memcpy(out, value.data(), value.size());
  }

  _data.addSize(extraLen);
  adjustBodyStart(static_cast<int64_t>(extraLen));

  return *this;
}

void HttpResponse::makeAllHeaderNamesLowerCase() {
  char* headersBeg = _data.data() + headersStartPos() + kDateHeaderLenWithCRLF + http::CRLF.size();
  char* headersEnd = _data.data() + bodyStartPos() - http::CRLF.size();

  while (headersBeg < headersEnd) {
    for (; *headersBeg != ':'; ++headersBeg) {
      *headersBeg = tolower(*headersBeg);
    }

    // move headersBeg to next header
    headersBeg = std::search(headersBeg + http::HeaderSep.size(), headersEnd, http::CRLF.begin(), http::CRLF.end()) +
                 http::CRLF.size();
  }
}

HttpPayload* HttpResponse::finalizeHeadersBody(http::Version version, SysTimePoint tp, bool isHeadMethod, bool close,
                                               const ConcatenatedHeaders& globalHeaders,
                                               std::size_t minCapturedBodySize) {
  HttpPayload* pExternPayload = externPayloadPtr();

  std::string_view externBodyAndTrailers;

  const auto bodySz = bodyLen();
  bool moveBodyInline;
  if (bodySz == 0) {
    assert(_trailerLen == 0);
    moveBodyInline = false;

    // erase body and trailers (but keep headers, especially Content-Length / Content-Type)
    if (hasNoCapturedNorFileBody()) {
      _data.setSize(_data.size() - bodySz);
    } else {
      _payloadVariant = {};
      pExternPayload = nullptr;
    }
  } else if (isHeadMethod) {
    // For HEAD responses we must not transmit the body, but keep file payloads
    // intact so ownership can be transferred by finalizeForHttp1. For inline
    // bodies we still erase the inline bytes.
    moveBodyInline = false;
    if (hasNoCapturedNorFileBody()) {
      _data.setSize(_data.size() - bodySz - _trailerLen);
    }
    // A HEAD response must not have trailers
    _trailerLen = 0;
  } else {
    moveBodyInline = pExternPayload != nullptr && bodySz <= minCapturedBodySize;
    if (moveBodyInline) {
      externBodyAndTrailers = pExternPayload->view();
    }
  }

  const std::string_view connectionValue = close ? http::close : http::keepalive;
  // HTTP/1.1 (RFC 7230 / RFC 9110) specifies that Connection: keep-alive is the default.
  // HTTP/1.0 is the opposite - Connection: close is the default.
  const bool addConnectionHeader = (close && version == http::HTTP_1_1) || (!close && version == http::HTTP_1_0);
  const bool hasHeaders = !headersFlatView().empty();

  std::size_t totalNewHeadersSize = 0;
  if (addConnectionHeader) {
    totalNewHeadersSize += HeaderSize(http::Connection.size(), connectionValue.size());
  }

  std::bitset<HttpServerConfig::kMaxGlobalHeaders> globalHeadersToSkipBmp;

  std::size_t pos = 0;
  bool writeAllGlobalHeaders = true;
  for (std::string_view headerKeyVal : globalHeaders) {
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

  assert(totalNewHeadersSize + bodyStartPos() <= kBodyStartMask);

  std::size_t extraSize = totalNewHeadersSize;
  if (moveBodyInline) {
    // we will move body into main buffer
    extraSize += externBodyAndTrailers.size();
  }
  if (_trailerLen != 0 && (moveBodyInline || pExternPayload == nullptr)) {
    // final CRLF after trailers
    extraSize += http::CRLF.size();
  }

  _data.ensureAvailableCapacity(extraSize);

  WriteCRLFDateHeader(_data.data() + headersStartPos(), tp);

  char* insertPtr = _data.data() + bodyStartPos() - http::DoubleCRLF.size();

  if (totalNewHeadersSize != 0) {
    std::memmove(insertPtr + totalNewHeadersSize, insertPtr, http::DoubleCRLF.size() + internalBodyAndTrailersLen());
  }

  if (!globalHeaders.empty()) {
    if (writeAllGlobalHeaders) {
      // Optim: single memcpy for all global headers
      std::memcpy(insertPtr, http::CRLF.data(), http::CRLF.size());
      const auto allGlobalHeaders = globalHeaders.fullString();
      std::memcpy(insertPtr + http::CRLF.size(), allGlobalHeaders.data(), allGlobalHeaders.size());
      insertPtr += allGlobalHeaders.size() + http::CRLF.size();
    } else {
      pos = 0;
      for (std::string_view headerKeyVal : globalHeaders) {
        if (!globalHeadersToSkipBmp.test(pos)) {
          std::memcpy(insertPtr, http::CRLF.data(), http::CRLF.size());
          std::memcpy(insertPtr + http::CRLF.size(), headerKeyVal.data(), headerKeyVal.size());
          insertPtr += headerKeyVal.size() + http::CRLF.size();
        }
        ++pos;
      }
    }
  }

  if (addConnectionHeader) {
    insertPtr = WriteCRLFHeader(insertPtr, http::Connection, connectionValue);
  }

  _data.addSize(extraSize);
  adjustBodyStart(static_cast<int64_t>(totalNewHeadersSize));

  if (moveBodyInline) {
    std::memcpy(_data.data() + bodyStartPos(), externBodyAndTrailers.data(), externBodyAndTrailers.size());
    _payloadVariant = {};
    pExternPayload = nullptr;
  }

  // We don't move large inline body sizes to the separated buffer, copy has already been done and we won't gain
  // anything.

  // Append trailers after body (RFC 7230 §4.1.2).
  // Trailers follow the body and are terminated by a final CRLF.
  // For chunked encoding, the server will emit the zero-length chunk (0\r\n) before trailers
  // during transmission (handled elsewhere in the streaming path or serialization layer).
  if (_trailerLen != 0) {
    // Final blank line terminates trailers
    if (pExternPayload != nullptr) {
      // In this case we need to append data to the external payload to keep order of HTTP data correct.
      pExternPayload->append(http::CRLF);
    } else {
      std::memcpy(_data.data() + _data.size() - http::CRLF.size(), http::CRLF.data(), http::CRLF.size());
    }
  }

  return pExternPayload;
}

HttpResponse& HttpResponse::trailerAddLine(std::string_view name, std::string_view value) & {
  assert(http::IsValidHeaderName(name));
  if (hasFileBody()) {
    throw std::logic_error("Cannot add trailers when response body uses sendfile");
  }
  if (bodyLen() == 0) {
    throw std::logic_error("Trailers must be added after non empty body is set");
  }

  const std::size_t lineSize = HeaderSize(name.size(), value.size());

  HttpPayload* pExternPayload = externPayloadPtr();
  char* insertPtr;
  if (pExternPayload != nullptr) {
    // Add an extra CRLF space for the last CRLF that will terminate trailers in finalize
    pExternPayload->ensureAvailableCapacityExponential(lineSize + http::CRLF.size());
    insertPtr = pExternPayload->data() + pExternPayload->size();
    pExternPayload->addSize(lineSize);
  } else {
    _data.ensureAvailableCapacityExponential(lineSize + http::CRLF.size());
    insertPtr = _data.data() + _data.size();
    _data.addSize(lineSize);
  }

  _trailerLen += lineSize;

  WriteHeaderCRLF(insertPtr, name, value);
  return *this;
}

HttpResponse::FormattedHttp1Response HttpResponse::finalizeForHttp1(http::Version version, SysTimePoint tp, bool close,
                                                                    const ConcatenatedHeaders& globalHeaders,
                                                                    bool isHeadMethod,
                                                                    std::size_t minCapturedBodySize) {
  const auto versionStr = version.str();
  std::memcpy(_data.data(), versionStr.data(), versionStr.size());

  HttpPayload* pExternPayload =
      finalizeHeadersBody(version, tp, isHeadMethod, close, globalHeaders, minCapturedBodySize);

  FormattedHttp1Response prepared;
  // Move head (_data) first.
  prepared.data = HttpResponseData{std::move(_data)};
  if (pExternPayload != nullptr) {
    // Move captured body out of the variant into a temporary HttpResponseData and append it.
    prepared.data.append(HttpResponseData{RawChars{}, std::move(*pExternPayload)});
  } else {
    FilePayload* pFilePayload = filePayloadPtr();
    if (pFilePayload != nullptr) {
      assert(pFilePayload->length != 0);
      prepared.file = std::move(pFilePayload->file);
      prepared.fileOffset = pFilePayload->offset;
      // HEAD responses must not transmit the body; still close the descriptor via moved File.
      prepared.fileLength = isHeadMethod ? 0 : pFilePayload->length;
    }
  }

  return prepared;
}

void HttpResponse::bodyAppendUpdateHeaders(std::string_view givenContentType, std::string_view defaultContentType,
                                           std::size_t totalBodyLen) {
  assert(_trailerLen == 0);
  const auto bodySz = bodyLen();
  const auto newBodyLenCharVec = IntegralToCharVector(totalBodyLen);
  if (bodySz == 0) {
    if (givenContentType.empty()) {
      headerAddLine(http::ContentType, defaultContentType);
    } else {
      headerAddLine(http::ContentType, givenContentType);
    }
    headerAddLine(http::ContentLength, std::string_view(newBodyLenCharVec));
  } else {
    if (!givenContentType.empty()) {
      replaceHeaderValueNoRealloc(getContentTypeValuePtr(bodySz), givenContentType);
    }
    replaceHeaderValueNoRealloc(getContentLengthValuePtr(bodySz), std::string_view(newBodyLenCharVec));
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
    _data.addSize(static_cast<std::size_t>(diff));
    adjustBodyStart(diff);
  }
  if (newValue.size() != 0) {
    std::memcpy(first, newValue.data(), newValue.size());
  }
}

}  // namespace aeronet
