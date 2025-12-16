#include "aeronet/http-response.hpp"

#include <algorithm>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
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
constexpr std::size_t HttpResponseInitialSize(std::string_view reason = {}) {
  return http::HTTP10Sv.size() + 1U + 3U + (reason.empty() ? 0UL : reason.size() + 1UL) + http::DoubleCRLF.size();
}
}  // namespace

HttpResponse::HttpResponse(http::StatusCode code, std::string_view reason)
    : HttpResponse(kHttpResponseMinInitialCapacity, code, reason) {}

HttpResponse::HttpResponse(std::size_t initialCapacity, http::StatusCode code, std::string_view reason)
    : _data(std::max({HttpResponseInitialSize(reason), initialCapacity, kHttpResponseMinInitialCapacity})),
      _bodyStartPos(static_cast<uint32_t>(HttpResponseInitialSize(reason))) {
  setStatusCode(code);
  if (!reason.empty()) {
    std::memcpy(_data.data() + kReasonBeg, reason.data(), reason.size());
  }
  std::memcpy(_data.data() + _bodyStartPos - http::DoubleCRLF.size(), http::DoubleCRLF.data(), http::DoubleCRLF.size());
  _data.setSize(_bodyStartPos);
}

HttpResponse::HttpResponse(std::string_view body, std::string_view contentType)
    : HttpResponse(HttpResponseInitialSize() + body.size(), http::StatusCodeOK, {}) {
  this->body(body, contentType);
}

std::size_t HttpResponse::reasonLen() const noexcept {
  if (_data[kReasonBeg] == '\n') {
    return 0UL;
  }
  if (_headersStartPos != 0) {
    return _headersStartPos - kReasonBeg;
  }
  return _bodyStartPos - kReasonBeg - http::DoubleCRLF.size();
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

void HttpResponse::setStatusCode(http::StatusCode statusCode) noexcept {
  assert(statusCode >= 100 && statusCode < 1000);
  write3(_data.data() + kStatusCodeBeg, statusCode);
}

std::string_view HttpResponse::internalTrailers() const noexcept {
  return {_trailerPos != 0 ? (_data.begin() + _bodyStartPos + _trailerPos) : _data.end(), _data.end()};
}

std::string_view HttpResponse::externalTrailers(const HttpPayload& data) const noexcept {
  auto bodyAndTrailers = data.view();
  return {_trailerPos != 0 ? (bodyAndTrailers.begin() + _trailerPos) : bodyAndTrailers.end(), bodyAndTrailers.end()};
}

void HttpResponse::setReason(std::string_view newReason) {
  static constexpr std::string_view::size_type kMaxReasonLength = 1024;
  if (newReason.size() > kMaxReasonLength) {
    log::error("Provided reason is too long ({} bytes), truncating it to {} bytes", newReason.length(),
               kMaxReasonLength);
    newReason.remove_suffix(newReason.size() - kMaxReasonLength);
  }

  const auto oldReasonSz = reasonLen();
  int32_t diff = static_cast<int32_t>(newReason.size()) - static_cast<int32_t>(oldReasonSz);
  if (diff > 0) {
    if (oldReasonSz == 0) {
      ++diff;  // for the space before first reason char
    }
    _data.ensureAvailableCapacityExponential(static_cast<std::size_t>(diff));
  } else if (diff < 0 && newReason.empty()) {
    --diff;  // remove the space that previously separated status and reason
  }
  const std::string_view oldReason = reason();
  char* orig = _data.data() + kReasonBeg + oldReason.size();
  if (oldReason.empty()) {
    --orig;  // point to the CR (space placeholder location)
  }
  std::memmove(orig + diff, orig,
               _data.size() - kStatusCodeBeg - 3U - oldReason.size() - static_cast<uint32_t>(!oldReason.empty()));
  _bodyStartPos = static_cast<uint32_t>(static_cast<int64_t>(_bodyStartPos) + diff);
  if (_headersStartPos != 0) {
    _headersStartPos = static_cast<decltype(_headersStartPos)>(static_cast<int64_t>(_headersStartPos) + diff);
  }
  std::memcpy(_data.data() + kReasonBeg, newReason.data(), newReason.size());
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

  _bodyStartPos = static_cast<uint32_t>(static_cast<int64_t>(_bodyStartPos) + diff);
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
  if (_headersStartPos + headerLineLen + http::DoubleCRLF.size() == _bodyStartPos) {
    _headersStartPos = 0;
  }

  _bodyStartPos -= static_cast<decltype(_bodyStartPos)>(headerLineLen);
  _data.setSize(_data.size() - headerLineLen);
}

void HttpResponse::setBodyInternal(std::string_view newBody) {
  setBodyEnsureNoTrailers();
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
    std::memcpy(_data.data() + _bodyStartPos, newBody.data(), newBody.size());
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
  setBodyEnsureNoTrailers();
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
  setBodyEnsureNoTrailers();

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
  setBodyEnsureNoTrailers();
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
  auto ret =
      pExternPayload != nullptr ? pExternPayload->view() : std::string_view{_data.begin() + _bodyStartPos, _data.end()};
  if (_trailerPos != 0) {
    ret.remove_suffix(ret.size() - _trailerPos);
  }
  return ret;
}

std::optional<std::string_view> HttpResponse::headerValue(std::string_view key) const noexcept {
  std::optional<std::string_view> ret;
  if (_headersStartPos == 0) {
    return ret;
  }

  const char* headersBeg = _data.data() + _headersStartPos + http::CRLF.size();
  const char* headersEnd = _data.data() + _bodyStartPos - http::CRLF.size();
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

  if (_headersStartPos == 0) {
    _headersStartPos = static_cast<decltype(_headersStartPos)>(_bodyStartPos - http::DoubleCRLF.size());
  }

  const std::size_t headerLineSize = http::CRLF.size() + key.size() + http::HeaderSep.size() + value.size();

  _data.ensureAvailableCapacityExponential(headerLineSize);

  char* insertPtr = _data.data() + _bodyStartPos - http::DoubleCRLF.size();

  std::memmove(insertPtr + headerLineSize, insertPtr, http::DoubleCRLF.size() + internalBodyAndTrailersLen());

  WriteCRLFHeader(insertPtr, key, value);

  _data.addSize(headerLineSize);
  _bodyStartPos += static_cast<uint32_t>(headerLineSize);
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
  _bodyStartPos = static_cast<uint32_t>(_bodyStartPos + extraLen);
}

HttpPayload* HttpResponse::finalizeHeadersBody(http::Version version, SysTimePoint tp, bool isHeadMethod, bool close,
                                               const ConcatenatedHeaders& globalHeaders,
                                               std::size_t minCapturedBodySize) {
  static constexpr std::size_t kHeaderAdditionalSize = http::CRLF.size() + http::HeaderSep.size();

  HttpPayload* pExternPayload = externPayloadPtr();

  std::string_view externBodyAndTrailers;

  const auto bodySz = bodyLen();
  bool moveBodyInline;
  if (bodySz == 0 || isHeadMethod) {
    moveBodyInline = false;

    // erase body and trailers (but keep headers, especially Content-Length / Content-Type)
    if (isInlineBody()) {
      _data.setSize(_data.size() - bodySz - _trailerPos);
    } else {
      _payloadVariant = {};
      pExternPayload = nullptr;
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
  const bool hasHeaders = _headersStartPos != 0;

  std::size_t totalNewHeadersSize = http::Date.size() + kRFC7231DateStrLen + kHeaderAdditionalSize;

  if (!hasHeaders) {
    _headersStartPos = static_cast<decltype(_headersStartPos)>(_bodyStartPos - http::DoubleCRLF.size());
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

  if (std::cmp_greater(totalNewHeadersSize + _bodyStartPos, std::numeric_limits<decltype(_bodyStartPos)>::max())) {
    throw std::length_error("HTTP response headers too large");
  }

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

  char* insertPtr = _data.data() + _bodyStartPos - http::DoubleCRLF.size();

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
  _bodyStartPos += static_cast<uint32_t>(totalNewHeadersSize);

  if (moveBodyInline) {
    std::memcpy(_data.data() + _bodyStartPos, externBodyAndTrailers.data(), externBodyAndTrailers.size());
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

HttpResponse::PreparedResponse HttpResponse::finalizeAndStealData(http::Version version, SysTimePoint tp, bool close,
                                                                  const ConcatenatedHeaders& globalHeaders,
                                                                  bool isHeadMethod, std::size_t minCapturedBodySize) {
  const auto versionStr = version.str();
  std::memcpy(_data.data(), versionStr.data(), versionStr.size());
  _data[versionStr.size()] = ' ';
  // status code already set. Space before reason only if reason present.
  const auto rLen = reasonLen();
  if (rLen != 0) {
    _data[kReasonBeg - 1UL] = ' ';
  }

  HttpPayload* pExternPayload =
      finalizeHeadersBody(version, tp, isHeadMethod, close, globalHeaders, minCapturedBodySize);

  PreparedResponse prepared;
  // Move head (_data) first.
  prepared.data = HttpResponseData{std::move(_data)};
  if (pExternPayload != nullptr) {
    // Move captured body out of the variant into a temporary HttpResponseData and append it.
    HttpPayload moved = std::move(*pExternPayload);
    HttpResponseData tail{RawChars{}, std::move(moved)};
    prepared.data.append(std::move(tail));
  } else {
    FilePayload* pFilePayload = filePayloadPtr();
    if (pFilePayload != nullptr && pFilePayload->length != 0) {
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
