#include "aeronet/http-response.hpp"

#include <algorithm>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
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
#include "aeronet/simple-charconv.hpp"
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

constexpr std::size_t HttpResponseInitialSize() {
  return http::HTTP10Sv.size() + 1U + kNdigitsStatusCode + http::DoubleCRLF.size();
}

constexpr std::size_t HttpResponseInitialSize(std::size_t reasonLen) {
  return http::HTTP10Sv.size() + 1U + kNdigitsStatusCode +
         (reasonLen == 0 ? 0UL : std::min(reasonLen, kMaxReasonLength) + 1UL) + http::DoubleCRLF.size();
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

HttpResponse::HttpResponse(std::size_t initialCapacity, http::StatusCode code)
    : _data(std::max({HttpResponseInitialSize(), initialCapacity, kHttpResponseMinInitialCapacity})) {
  _data[http::HTTP10Sv.size()] = ' ';
  const auto bodyStart = HttpResponseInitialSize();
  setBodyStartPos(bodyStart);
  setStatusCode(code);
  std::memcpy(_data.data() + bodyStart - http::DoubleCRLF.size(), http::DoubleCRLF.data(), http::DoubleCRLF.size());
  _data.setSize(bodyStart);
}

HttpResponse::HttpResponse(std::size_t initialCapacity, http::StatusCode code, std::string_view reason)
    : _data(std::max({HttpResponseInitialSize(reason.size()), initialCapacity, kHttpResponseMinInitialCapacity})) {
  _data[http::HTTP10Sv.size()] = ' ';
  const auto bodyStart = HttpResponseInitialSize(reason.size());
  setBodyStartPos(bodyStart);
  setStatusCode(code);
  if (!reason.empty()) {
    reason = AdjustReasonLen(reason);
    _data[kReasonBeg - 1UL] = ' ';
    std::memcpy(_data.data() + kReasonBeg, reason.data(), reason.size());
  }
  std::memcpy(_data.data() + bodyStart - http::DoubleCRLF.size(), http::DoubleCRLF.data(), http::DoubleCRLF.size());
  _data.setSize(bodyStart);
}

HttpResponse::HttpResponse(std::string_view body, std::string_view contentType)
    : HttpResponse(HttpResponseInitialSize() + body.size(), http::StatusCodeOK, {}) {
  this->body(body, contentType);
}

std::size_t HttpResponse::reasonLen() const noexcept {
  if (_data[kReasonBeg] == '\n') {
    return 0UL;
  }
  if (headersStartPos() != 0) {
    return headersStartPos() - kReasonBeg;
  }
  return bodyStartPos() - kReasonBeg - http::DoubleCRLF.size();
}

std::size_t HttpResponse::bodyLen() const noexcept {
  if (const FilePayload* pFilePayload = filePayloadPtr(); pFilePayload != nullptr) {
    return static_cast<std::size_t>(pFilePayload->length);
  }
  if (_trailerPos != 0) {
    return _trailerPos;
  }
  const HttpPayload* pExternPayload = externPayloadPtr();
  return pExternPayload != nullptr ? pExternPayload->size() : internalBodyAndTrailersLen();
}

void HttpResponse::setStatusCode(http::StatusCode statusCode) {
  if (statusCode < 100 || statusCode > 999) [[unlikely]] {
    throw std::invalid_argument("Invalid HTTP status code, should be 3 digits");
  }
  write3(_data.data() + kStatusCodeBeg, statusCode);
}

std::string_view HttpResponse::internalTrailers() const noexcept {
  return {_trailerPos != 0 ? (_data.begin() + bodyStartPos() + _trailerPos) : _data.end(), _data.end()};
}

std::string_view HttpResponse::externalTrailers(const HttpPayload& data) const noexcept {
  auto bodyAndTrailers = data.view();
  return {_trailerPos != 0 ? (bodyAndTrailers.begin() + _trailerPos) : bodyAndTrailers.end(), bodyAndTrailers.end()};
}

void HttpResponse::setReason(std::string_view newReason) {
  newReason = AdjustReasonLen(newReason);
  auto oldReasonSz = reasonLen();
  int32_t diff = static_cast<int32_t>(newReason.size()) - static_cast<int32_t>(oldReasonSz);
  if (diff == 0) {
    if (!newReason.empty()) {
      std::memcpy(_data.data() + kReasonBeg, newReason.data(), newReason.size());
    }
    return;
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
  if (headersStartPos() != 0) {
    adjustHeadersStart(diff);
  }
  if (!newReason.empty()) {
    _data[kReasonBeg - 1UL] = ' ';
    std::memcpy(_data.data() + kReasonBeg, newReason.data(), newReason.size());
  }
  _data.addSize(static_cast<std::size_t>(diff));
}

void HttpResponse::setHeader(std::string_view newKey, std::string_view newValue, OnlyIfNew onlyIfNew) {
  assert(http::IsValidHeaderName(newKey));

  auto optValue = headerValue(newKey);
  if (!optValue) {
    appendHeaderInternal(newKey, newValue);
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

void HttpResponse::setContentTypeHeader(std::string_view contentTypeValue, bool isEmpty) {
  if (isEmpty) {
    eraseHeader(http::ContentType);
  } else {
    setHeader(http::ContentType, contentTypeValue);
  }
}

void HttpResponse::eraseHeader(std::string_view key) {
  auto optHeaderValue = headerValue(key);
  if (!optHeaderValue) {
    return;
  }

  const char* valueEnd = optHeaderValue->end();
  char* keyBeg = _data.data() + (optHeaderValue->data() - _data.data()) - static_cast<std::ptrdiff_t>(key.size()) -
                 http::HeaderSep.size();

  std::memmove(keyBeg - http::CRLF.size(), valueEnd, static_cast<std::size_t>(_data.end() - valueEnd));

  const std::size_t headerLineLen = static_cast<std::size_t>(valueEnd - keyBeg) + http::CRLF.size();

  // If removed header was the only header, we need to reset _headersStartPos to 0
  if (headersStartPos() + headerLineLen + http::DoubleCRLF.size() == bodyStartPos()) {
    setHeadersStartPos(0);
  }

  adjustBodyStart(-static_cast<int64_t>(headerLineLen));
  _data.setSize(_data.size() - headerLineLen);
}

namespace {
void SetBodyEnsureNoTrailers(std::size_t trailerPos) {
  if (trailerPos != 0) [[unlikely]] {
    throw std::logic_error("Cannot set body after the first trailer");
  }
}

}  // namespace

void HttpResponse::setBodyInternal(std::string_view newBody) {
  SetBodyEnsureNoTrailers(_trailerPos);
  const int64_t diff = static_cast<int64_t>(newBody.size()) - static_cast<int64_t>(internalBodyAndTrailersLen());
  if (diff > 0) {
    int64_t newBodyInternalPos = -1;
    if (newBody.data() > _data.data() && newBody.data() <= _data.data() + _data.size()) {
      // the memory pointed by newBody is internal to HttpResponse. We need to save the position before
      // realloc
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

namespace {
// Even if this space will be unused now, it will anyway still be needed for the finalization of the HttpResponse.
constexpr std::size_t kExtraSpaceForContentTypeHeader =
    http::CRLF.size() + http::ContentType.size() + http::HeaderSep.size();
}  // namespace

void HttpResponse::appendBodyInternal(std::string_view data, std::string_view contentType) {
  SetBodyEnsureNoTrailers(_trailerPos);

  if (!data.empty()) {
    _data.ensureAvailableCapacityExponential(
        data.size() + kExtraSpaceForContentTypeHeader +
        std::max(http::ContentTypeApplicationOctetStream.size(), contentType.size()));

    _data.unchecked_append(data);

    appendBodyResetContentType(contentType, http::ContentTypeTextPlain);
  }
  _payloadVariant = {};
}

void HttpResponse::appendBodyInternal(std::size_t maxLen, const std::function<std::size_t(char*)>& writer,
                                      std::string_view contentType) {
  SetBodyEnsureNoTrailers(_trailerPos);

  _data.ensureAvailableCapacityExponential(maxLen + kExtraSpaceForContentTypeHeader +
                                           std::max(http::ContentTypeTextPlain.size(), contentType.size()));

  const auto written = writer(_data.data() + _data.size());

  if (written != 0) {
    _data.addSize(written);

    appendBodyResetContentType(contentType, http::ContentTypeTextPlain);
  }
  _payloadVariant = {};
}

void HttpResponse::appendBodyInternal(std::size_t maxLen, const std::function<std::size_t(std::byte*)>& writer,
                                      std::string_view contentType) {
  SetBodyEnsureNoTrailers(_trailerPos);

  _data.ensureAvailableCapacityExponential(
      maxLen + kExtraSpaceForContentTypeHeader +
      std::max(http::ContentTypeApplicationOctetStream.size(), contentType.size()));

  const auto written = writer(reinterpret_cast<std::byte*>(_data.data()) + _data.size());

  if (written != 0) {
    _data.addSize(written);

    appendBodyResetContentType(contentType, http::ContentTypeApplicationOctetStream);
  }
  _payloadVariant = {};
}

HttpResponse& HttpResponse::file(File fileObj, std::size_t offset, std::size_t length, std::string_view contentType) & {
  if (!fileObj) {
    throw std::invalid_argument("file requires an opened file");
  }
  const std::size_t fileSize = fileObj.size();
  if (offset > fileSize) {
    throw std::invalid_argument("file offset exceeds file size");
  }
  const std::size_t resolvedLength = length == 0 ? (fileSize - offset) : length;
  if (offset + resolvedLength > fileSize) {
    throw std::invalid_argument("file length exceeds file size");
  }
  if (_trailerPos != 0) {
    throw std::logic_error("Cannot call file after adding trailers");
  }
  if (contentType.empty()) {
    setHeader(http::ContentType, fileObj.detectedContentType());
  } else {
    setHeader(http::ContentType, contentType);
  }
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
  if (_trailerPos != 0) {
    ret.remove_suffix(ret.size() - _trailerPos);
  }
  return ret;
}

std::string_view HttpResponse::headersFlatView() const noexcept {
  const auto headersStart = headersStartPos();
  if (headersStart == 0) {
    return {};
  }
  return {_data.data() + headersStart + http::CRLF.size(), _data.data() + bodyStartPos() - http::CRLF.size()};
}

HttpResponse::HeadersView::iterator::iterator(const char* beg, const char* end) noexcept : _cur(beg), _end(end) {
  if (_cur != _end) {
    const char* colonPtr = std::search(_cur, _end, http::HeaderSep.begin(), http::HeaderSep.end());
    assert(colonPtr != _end);
    _colonPos = static_cast<uint32_t>(colonPtr - _cur);

    const char* crlfPtr = std::search(colonPtr + http::HeaderSep.size(), _end, http::CRLF.begin(), http::CRLF.end());
    _crlfPos = static_cast<uint32_t>(crlfPtr - _cur);
  }
}

HttpResponse::HeadersView::iterator& HttpResponse::HeadersView::iterator::operator++() noexcept {
  _cur += _crlfPos + http::CRLF.size();

  const char* colonPtr = std::search(_cur, _end, http::HeaderSep.begin(), http::HeaderSep.end());
  _colonPos = static_cast<uint32_t>(colonPtr - _cur);
  const char* crlfPtr = std::search(colonPtr + http::HeaderSep.size(), _end, http::CRLF.begin(), http::CRLF.end());
  _crlfPos = static_cast<uint32_t>(crlfPtr - _cur);

  return *this;
}

std::optional<std::string_view> HttpResponse::headerValue(std::string_view key) const noexcept {
  std::optional<std::string_view> ret;
  const auto headersStart = headersStartPos();
  if (headersStart == 0) {
    return ret;
  }

  const char* headersBeg = _data.data() + headersStart + http::CRLF.size();
  const char* headersEnd = _data.data() + bodyStartPos() - http::CRLF.size();
  const char* endKey = key.end();

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
        std::search(headersBeg + http::CRLF.size(), headersEnd, http::CRLF.begin(), http::CRLF.end());

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

void HttpResponse::appendHeaderInternal(std::string_view key, std::string_view value) {
  assert(http::IsValidHeaderName(key));

  if (headersStartPos() == 0) {
    setHeadersStartPos(static_cast<std::uint16_t>(bodyStartPos() - http::DoubleCRLF.size()));
  }

  const std::size_t headerLineSize = http::CRLF.size() + key.size() + http::HeaderSep.size() + value.size();

  _data.ensureAvailableCapacityExponential(headerLineSize);

  char* insertPtr = _data.data() + bodyStartPos() - http::DoubleCRLF.size();

  std::memmove(insertPtr + headerLineSize, insertPtr, http::DoubleCRLF.size() + internalBodyAndTrailersLen());

  WriteCRLFHeader(insertPtr, key, value);

  _data.addSize(headerLineSize);
  adjustBodyStart(static_cast<int64_t>(headerLineSize));
}

void HttpResponse::appendHeaderValueInternal(std::string_view key, std::string_view value, std::string_view separator) {
  assert(!http::IsReservedResponseHeader(key));
  assert(http::IsValidHeaderName(key));

  auto optValue = headerValue(key);
  if (!optValue) {
    appendHeaderInternal(key, value);
    return;
  }

  const std::size_t extraLen = separator.size() + value.size();
  if (extraLen == 0) {
    return;
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
}

HttpPayload* HttpResponse::finalizeHeadersBody(http::Version version, SysTimePoint tp, bool isHeadMethod, bool close,
                                               const ConcatenatedHeaders& globalHeaders,
                                               std::size_t minCapturedBodySize) {
  static constexpr std::size_t kHeaderAdditionalSize = http::CRLF.size() + http::HeaderSep.size();

  HttpPayload* pExternPayload = externPayloadPtr();

  std::string_view externBodyAndTrailers;

  const auto bodySz = bodyLen();
  bool moveBodyInline;
  if (bodySz == 0) {
    assert(_trailerPos == 0);
    moveBodyInline = false;

    // erase body and trailers (but keep headers, especially Content-Length / Content-Type)
    if (isInlineBody()) {
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
    if (isInlineBody()) {
      _data.setSize(_data.size() - bodySz - _trailerPos);
    }
    // A HEAD response must not have trailers
    _trailerPos = 0;
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
  const auto bodySzStr = IntegralToCharVector(bodySz);
  const bool hasHeaders = headersStartPos() != 0;

  std::size_t totalNewHeadersSize = http::Date.size() + kRFC7231DateStrLen + kHeaderAdditionalSize;

  if (!hasHeaders) {
    setHeadersStartPos(static_cast<std::uint16_t>(bodyStartPos() - http::DoubleCRLF.size()));
  }

  if (addConnectionHeader) {
    totalNewHeadersSize += http::Connection.size() + connectionValue.size() + kHeaderAdditionalSize;
  }

  if (bodySz != 0) {
    totalNewHeadersSize += http::ContentLength.size() + bodySzStr.size() + kHeaderAdditionalSize;
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
  if (_trailerPos != 0 && (moveBodyInline || pExternPayload == nullptr)) {
    // final CRLF after trailers
    extraSize += http::CRLF.size();
  }

  _data.ensureAvailableCapacity(extraSize);

  char* insertPtr = _data.data() + bodyStartPos() - http::DoubleCRLF.size();

  std::memmove(insertPtr + totalNewHeadersSize, insertPtr, http::DoubleCRLF.size() + internalBodyAndTrailersLen());

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
  insertPtr = WriteCRLFDateHeader(insertPtr, tp);
  if (bodySz != 0) {
    insertPtr = WriteCRLFHeader(insertPtr, http::ContentLength, std::string_view(bodySzStr));
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
  if (_trailerPos != 0) {
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

void HttpResponse::appendTrailer(std::string_view name, std::string_view value) {
  assert(http::IsValidHeaderName(name));
  if (isFileBody()) {
    throw std::logic_error("Cannot add trailers when response body uses sendfile");
  }
  if (bodyLen() == 0) {
    throw std::logic_error("Trailers must be added after non empty body is set");
  }

  // Trailer format: name ": " value CRLF
  const std::size_t lineSize = name.size() + http::HeaderSep.size() + value.size() + http::CRLF.size();

  HttpPayload* pExternPayload = externPayloadPtr();
  char* insertPtr;
  if (pExternPayload != nullptr) {
    // Add an extra CRLF space for the last CRLF that will terminate trailers in finalize
    pExternPayload->ensureAvailableCapacityExponential(lineSize + http::CRLF.size());
    insertPtr = pExternPayload->data() + pExternPayload->size();
    if (_trailerPos == 0) {
      // store trailer position relative to the start of the body (captured body case)
      _trailerPos = pExternPayload->size();
    }
    pExternPayload->addSize(lineSize);
  } else {
    _data.ensureAvailableCapacityExponential(lineSize + http::CRLF.size());
    insertPtr = _data.data() + _data.size();
    if (_trailerPos == 0) {
      // _trailerPos is stored relative to the start of the body. For inline bodies the
      // body begins at _bodyStartPos so compute the relative offset here.
      _trailerPos = internalBodyAndTrailersLen();
    }
    _data.addSize(lineSize);
  }

  WriteHeaderCRLF(insertPtr, name, value);
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

void HttpResponse::appendBodyResetContentType(std::string_view givenContentType, std::string_view defaultContentType) {
  if (givenContentType.empty()) {
    // If we previously had a captured payload, overwrite any existing Content-Type.
    setHeader(http::ContentType, defaultContentType, isInlineBody() ? OnlyIfNew::Yes : OnlyIfNew::No);
  } else {
    setHeader(http::ContentType, givenContentType, OnlyIfNew::No);
  }
}

}  // namespace aeronet
