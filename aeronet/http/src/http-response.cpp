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

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/concatenated-headers.hpp"
#include "aeronet/file.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
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
// but in practice it should be reasonable. It's not really used by clients,
// as they mostly rely on the status code instead.
constexpr std::string_view::size_type kMaxReasonLength = 1024;

// Number of digits in the status code (3 digits).
constexpr std::size_t kStatusCodeLen = 3U;

// Date header will always be present at headersStartPos.
constexpr std::size_t kDateHeaderLenWithCRLF = HttpResponse::HeaderSize(http::Date.size(), kRFC7231DateStrLen);

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
  // In debug, this allows for easier inspection of the response data.
  http::HTTP_1_1.writeFull(data);
  WriteCRLFDateHeader(data + kStatusLineMinLenWithoutCRLF, SysClock::now());
#endif
}

constexpr std::size_t NeededBodyHeadersSize(std::size_t bodySize, std::size_t contentTypeSize) {
  if (bodySize == 0) {
    return 0;
  }
  return HttpResponse::HeaderSize(http::ContentType.size(), contentTypeSize) +
         HttpResponse::HeaderSize(http::ContentLength.size(), static_cast<std::size_t>(nchars(bodySize)));
}

constexpr std::string_view kTrailerValueSep = ", ";

constexpr std::string_view kTransferEncodingChunkedCRLF =
    JoinStringView_v<http::TransferEncoding, http::HeaderSep, http::chunked, http::CRLF>;
constexpr std::string_view kEndChunkedBody = "\r\n0\r\n";

void Copy(std::string_view sv, char* dst) noexcept { std::memcpy(dst, sv.data(), sv.size()); }

// Using std::memcpy for better performance
[[nodiscard]] constexpr char* Append(std::string_view sv, char* dst) {
  Copy(sv, dst);
  return dst + sv.size();
}

// Returns the size difference between the new Transfer-Encoding: chunked header.
// For very large payloads, this can be negative, as Content-Length can be larger than
// Transfer-Encoding: chunked.
constexpr int64_t TransferEncodingHeaderSizeDiff(std::size_t bodySz) {
  const auto oldContentLengthHeaderSize =
      HttpResponse::HeaderSize(http::ContentLength.size(), static_cast<std::size_t>(nchars(bodySz)));

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

}  // namespace

HttpResponse::HttpResponse(http::StatusCode code, std::string_view body, std::string_view contentType)
    : _data(kHttpResponseInitialSize + NeededBodyHeadersSize(body.size(), contentType.size()) + body.size()) {
  InitData(_data.data());
  status(code);
  setHeadersStartPos(static_cast<std::uint16_t>(kStatusLineMinLenWithoutCRLF));
  if (body.empty()) {
    setBodyStartPos(kInitialBodyStart);
    Copy(http::DoubleCRLF, _data.data() + kInitialBodyStart - http::DoubleCRLF.size());
    _data.setSize(kInitialBodyStart);
  } else {
    char* insertPtr = WriteCRLFHeader(_data.data() + kStatusLineMinLenWithoutCRLF + kDateHeaderLenWithCRLF,
                                      http::ContentType, contentType);
    insertPtr = WriteCRLFHeader(insertPtr, http::ContentLength, body.size());
    insertPtr = Append(http::DoubleCRLF, insertPtr);
    const auto bodyStartPos = static_cast<std::uint64_t>(insertPtr - _data.data());
    setBodyStartPos(bodyStartPos);
    Copy(body, insertPtr);
    _data.setSize(bodyStartPos + body.size());
  }
}

HttpResponse::HttpResponse(std::size_t additionalCapacity, http::StatusCode code)
    : _data(kHttpResponseInitialSize + additionalCapacity) {
  InitData(_data.data());
  status(code);
  setHeadersStartPos(static_cast<std::uint16_t>(kStatusLineMinLenWithoutCRLF));
  setBodyStartPos(kInitialBodyStart);
  Copy(http::DoubleCRLF, _data.data() + kInitialBodyStart - http::DoubleCRLF.size());
  _data.setSize(kInitialBodyStart);
}

HttpResponse::HttpResponse(std::size_t additionalCapacity, http::StatusCode code, std::string_view concatenatedHeaders,
                           std::string_view body, std::string_view contentType)
    : _data(kHttpResponseInitialSize + concatenatedHeaders.size() +
            NeededBodyHeadersSize(body.size(), contentType.size()) + body.size() + additionalCapacity) {
  assert(concatenatedHeaders.empty() || concatenatedHeaders.ends_with(http::CRLF));
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
    char* insertPtr = WriteCRLFHeader(_data.data() + bodyStartPos - http::CRLF.size(), http::ContentType, contentType);
    insertPtr = WriteCRLFHeader(insertPtr, http::ContentLength, body.size());
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
  _data.addSize(static_cast<std::size_t>(diff));
  return *this;
}

bool HttpResponse::setHeader(std::string_view newKey, std::string_view newValue, OnlyIfNew onlyIfNew) {
  assert(http::IsValidHeaderName(newKey));

  auto optValue = headerValue(newKey);
  if (!optValue) {
    headerAddLine(newKey, newValue);
    return true;
  }
  if (onlyIfNew == OnlyIfNew::Yes) {
    return false;
  }

  newValue = TrimOws(newValue);

  char* valueFirst = _data.data() + (optValue->data() - _data.data());
  const std::size_t oldHeaderValueSz = optValue->size();

  const auto diff = static_cast<int64_t>(newValue.size()) - static_cast<int64_t>(oldHeaderValueSz);
  if (diff == 0) {
    Copy(newValue, valueFirst);
    return true;
  }

  const auto valuePos = static_cast<std::size_t>(valueFirst - _data.data());
  if (diff > 0) {
    _data.ensureAvailableCapacityExponential(static_cast<uint64_t>(diff));
    valueFirst = _data.data() + valuePos;
  }

  std::memmove(valueFirst + newValue.size(), valueFirst + oldHeaderValueSz, _data.size() - valuePos - oldHeaderValueSz);
  Copy(newValue, valueFirst);

  // Works even if diff is negative, the unsigned value will overflow and have a resulting value of exactly sz - diff.
  // In C++, unsigned overflow is well-defined.
  _data.addSize(static_cast<std::size_t>(diff));

  adjustBodyStart(diff);

  return true;
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
  contentTypeValue = TrimOws(contentTypeValue);
  if (contentTypeValue.empty() && newBodySize != 0) [[unlikely]] {
    throw std::invalid_argument("Content-Type value cannot be empty for non-empty body");
  }

  const auto oldBodyLen = bodyLength();
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
    const auto newBodyLenCharVec = IntegralToCharVector(newBodySize);
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
  // Capacity should already have been ensured in setBodyHeaders
  assert(_data.size() + static_cast<std::size_t>(diff) <= _data.capacity());

  // should not point to internal memory
  assert(newBody.empty() || newBody.data() <= _data.data() || newBody.data() > _data.data() + _data.size());

  _data.addSize(static_cast<std::size_t>(diff));
  if (!newBody.empty()) {
    Copy(newBody, _data.data() + bodyStartPos());
  }
  // Clear payload variant at the end because newBody may point to its internal memory.
  _payloadVariant = {};
}

HttpResponse& HttpResponse::bodyAppend(std::string_view body, std::string_view contentType) & {
  if (!body.empty()) {
    if (hasBodyFile()) [[unlikely]] {
      throw std::logic_error("Cannot append to a captured file body");
    }
    if (hasBodyCaptured()) {
      if (!contentType.empty()) {
        setHeader(http::ContentType, contentType);
      }
      _payloadVariant.append(body);
    } else {
      const bool setContentTypeIfPresent = !contentType.empty();

      if (contentType.empty()) {
        contentType = http::ContentTypeTextPlain;
      }

      setBodyHeaders(contentType, internalBodyAndTrailersLen() + body.size(), setContentTypeIfPresent);

      _data.unchecked_append(body);
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
  _payloadVariant = HttpPayload(FilePayload(std::move(fileObj), offset, resolvedLength));
  return *this;
}

const File* HttpResponse::file() const noexcept {
  const auto* pFilePayload = _payloadVariant.getIfFilePayload();
  return pFilePayload == nullptr ? nullptr : &pFilePayload->file;
}

std::string_view HttpResponse::bodyInMemory() const noexcept {
  auto ret = hasBodyCaptured() ? _payloadVariant.view() : std::string_view{_data.begin() + bodyStartPos(), _data.end()};
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

  value = TrimOws(value);

  const std::size_t headerLineSize = HeaderSize(key.size(), value.size());

  _data.ensureAvailableCapacityExponential(headerLineSize);

  char* insertPtr = _data.data() + bodyStartPos() - http::DoubleCRLF.size();

  const auto bodySz = bodyLength();

  if (bodySz == 0) {
    std::memmove(insertPtr + headerLineSize, insertPtr, http::DoubleCRLF.size());
  } else {
    // We want to keep Content-Type and Content-Length together with the body (we use this property for optimization)
    // so we insert new headers before them. Of course, this code takes time, but it should be rare to add headers
    // after setting the body, so we can consider this as a 'slow' path.
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

  value = TrimOws(value);

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
    out = Append(separator, out);
  }
  if (!value.empty()) {
    Copy(value, out);
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

HttpResponse& HttpResponse::trailerAddLine(std::string_view name, std::string_view value) & {
  assert(http::IsValidHeaderName(name) && !http::IsForbiddenTrailerHeader(name));
  if (!hasBodyInMemory()) {
    throw std::logic_error("Trailers must be added after non empty (nor file) body is set");
  }

  /*

  Example of a full HTTP/1.1 response with chunked transfer encoding and trailers:

  HTTP/1.1 200 OK\r\n
  Date: Tue, 12 Jan 2026 10:15:30 GMT\r\n
  Content-Type: text/plain\r\n
  Transfer-Encoding: chunked\r\n
  Trailer: Expires, Digest\r\n     <-- SHOULD, but not MUST, be present if trailers are used. TODO: implement this
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

  const std::size_t lineSize = HeaderSize(name.size(), value.size());
  const auto newTrailerLen = SafeCast<decltype(_trailerLen)>(lineSize + _trailerLen);

  // Optim - if we know that option is not set, no need to pre-reserve additional capacity for Trailer header
  const bool addTrailerHeader = !_knownOptions.isPrepared() || _knownOptions.isAddTrailerHeader();

  // Add an extra CRLF space for the last CRLF that will terminate trailers in finalize
  int64_t neededCapacity = static_cast<int64_t>(lineSize + http::CRLF.size());
  if (_trailerLen == 0) {
    if (addTrailerHeader) {
      neededCapacity += static_cast<int64_t>(HeaderSize(http::Trailer.size(), name.size()));
    }
    neededCapacity += TransferEncodingHeaderSizeDiff(bodyInMemoryLength());
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

  _trailerLen = newTrailerLen;

  WriteHeaderCRLF(insertPtr, name, value);
  return *this;
}

HttpResponseData HttpResponse::finalizeForHttp1(SysTimePoint tp, http::Version version, Options opts,
                                                const ConcatenatedHeaders* pGlobalHeaders,
                                                std::size_t minCapturedBodySize) {
  version.writeFull(_data.data());

  const auto bodySz = bodyLength();

  const std::string_view connectionValue = opts.isClose() ? http::close : http::keepalive;
  // HTTP/1.1 (RFC 7230 / RFC 9110) specifies that Connection: keep-alive is the default.
  // HTTP/1.0 is the opposite - Connection: close is the default.
  const bool addConnectionHeader =
      (opts.isClose() && version == http::HTTP_1_1) || (!opts.isClose() && version == http::HTTP_1_0);
  const bool hasHeaders = !headersFlatView().empty();
  const bool addTrailerHeader = _trailerLen != 0 && opts.isAddTrailerHeader();
  const bool isHeadMethod = opts.isHeadMethod();

  std::size_t totalNewHeadersSize =
      addConnectionHeader ? HeaderSize(http::Connection.size(), connectionValue.size()) : 0;

  std::bitset<HttpServerConfig::kMaxGlobalHeaders> globalHeadersToSkipBmp;

  bool writeAllGlobalHeaders = true;
  if (!_knownOptions.isPrepared()) {
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

  if (bodySz == 0 || (opts.isHeadMethod() && _trailerLen == 0) || hasBodyFile()) {
    // For HEAD responses we must not transmit the body, but keep file payloads
    // intact so ownership can be transferred by finalizeForHttp1. For inline
    // bodies we still erase the inline bytes.
    if (hasNoCapturedNorFileBody()) {
      _data.setSize(_data.size() - bodySz);
    }
    if (totalNewHeadersSize != 0) {
      _data.ensureAvailableCapacity(totalNewHeadersSize);
      headersInsertPtr = _data.data() + bodyStartPos() - http::DoubleCRLF.size();
      // Move \r\n\r\n to its final place
      std::memmove(headersInsertPtr + totalNewHeadersSize, headersInsertPtr, http::DoubleCRLF.size());
    }
  } else {
    // body > 0 && (!isHeadMethod || _trailerLen != 0) && !hasFileBody()
    bool moveBodyInline = hasBodyCaptured() && bodySz + _trailerLen <= minCapturedBodySize;
    if (_trailerLen == 0) {
      _data.ensureAvailableCapacity((moveBodyInline ? bodySz : 0) + totalNewHeadersSize);
      char* oldBodyStart = _data.data() + bodyStartPos();
      headersInsertPtr = oldBodyStart - http::DoubleCRLF.size();
      if (moveBodyInline) {
        if (totalNewHeadersSize != 0) {
          // Move \r\n\r\n to its final place
          std::memmove(headersInsertPtr + totalNewHeadersSize, headersInsertPtr, http::DoubleCRLF.size());
        }
        auto bodyAndTrailersView = _payloadVariant.view();
        Copy(bodyAndTrailersView, oldBodyStart + totalNewHeadersSize);

        _data.addSize(bodySz);

        _payloadVariant = {};
      } else if (totalNewHeadersSize != 0) {
        if (!hasBodyCaptured()) {
          // Move inline body to its final place
          std::memmove(oldBodyStart + totalNewHeadersSize, oldBodyStart, bodySz);
        }

        // Move \r\n\r\n to its final place
        std::memmove(headersInsertPtr + totalNewHeadersSize, headersInsertPtr, http::DoubleCRLF.size());
      }
    } else {
      // RFC 7230 §4.1.2: Trailers require chunked transfer encoding.
      // Convert response to chunked format before proceeding with finalization.
      // Note: HEAD responses don't transmit body/trailers.

      // Calculate size difference between Content-Length header and Transfer-Encoding: chunked header
      // Content-Length: N -> Transfer-Encoding: chunked
      // Old header size: CRLF + "content-length" + ": " + nchars(bodySz)
      // New header size: CRLF + "transfer-encoding" + ": " + "chunked"
      int64_t headerSizeDiff = TransferEncodingHeaderSizeDiff(bodySz);

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

        std::size_t neededSize = static_cast<std::size_t>(headerSizeDiff);
        if (!isHeadMethod) {
          neededSize += http::CRLF.size() + hexDigits;
          if (moveBodyInline) {
            neededSize += _payloadVariant.size() + kEndChunkedBody.size() + http::CRLF.size();
          }
        }

        _data.ensureAvailableCapacity(neededSize + totalNewHeadersSize);

        char* newDoubleCRLFPtr = _data.data() + bodyStartPos() - http::DoubleCRLF.size() + totalNewHeadersSize +
                                 static_cast<std::size_t>(headerSizeDiff);

        // Now update headers in _data: replace Content-Length with Transfer-Encoding: chunked
        // Find and replace Content-Length header
        headersInsertPtr = ReplaceContentLengthWithTransferEncoding(getContentLengthHeaderLinePtr(bodySz),
                                                                    trailersFlatView(), addTrailerHeader);

        if (isHeadMethod) {
          Copy(http::DoubleCRLF, headersInsertPtr + totalNewHeadersSize);
          _payloadVariant = {};
        } else {
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
            std::memcpy(insertPtr, bodyAndTrailersView.data() + bodySz, _trailerLen);
            insertPtr += _trailerLen;

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
        }
        _data.addSize(neededSize);
      } else {
        // Inline body case: body and trailers are in _data

        // Calculate new total size (including final CRLF after trailers)
        int64_t diffSz = headerSizeDiff;
        if (isHeadMethod) {
          diffSz -= static_cast<int64_t>(bodySz + _trailerLen);
        } else {
          diffSz += static_cast<int64_t>(hexDigits + kEndChunkedBody.size() + http::DoubleCRLF.size());
        }
        int64_t neededSize = diffSz + static_cast<int64_t>(totalNewHeadersSize);
        if (neededSize > 0) {
          _data.ensureAvailableCapacity(static_cast<std::size_t>(neededSize));
        }

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
        std::string_view newTrailersFlatView(_data.data() + newTrailersStart, static_cast<std::size_t>(_trailerLen));

        if (isHeadMethod) {
          newTrailersFlatView = trailersFlatView();
        } else {
          // Write final CRLF first (it's at the very end)
          Copy(http::CRLF, _data.data() + newTrailersStart + _trailerLen);

          // Move trailers (they go before final CRLF) - use offset, not old view
          std::memmove(_data.data() + newTrailersStart, _data.data() + oldTrailerStart, _trailerLen);

          // Write last-chunk "\r\n0\r\n"
          Copy(kEndChunkedBody, _data.data() + newLastChunkStart - http::CRLF.size());

          // Move body data - use offset, not old view
          std::memmove(_data.data() + newBodyDataStart, _data.data() + oldBodyStart, bodySz);

          // Write chunk header "hex(len)\r\n"
          Copy(http::CRLF, to_lower_hex(bodySz, _data.data() + newHexStart));
        }

        // Move and update headers: replace Content-Length with Transfer-Encoding: chunked
        // Move everything before body (headers and DoubleCRLF)
        // Find Content-Length header position (relative to start)
        headersInsertPtr = ReplaceContentLengthWithTransferEncoding(getContentLengthHeaderLinePtr(bodySz),
                                                                    newTrailersFlatView, addTrailerHeader);

        Copy(http::DoubleCRLF, headersInsertPtr + totalNewHeadersSize);

        // Update buffer size and body start position
        _data.addSize(static_cast<std::size_t>(diffSz));
      }
    }
  }

  if (!_knownOptions.isPrepared() && !pGlobalHeaders->empty()) {
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
    headersInsertPtr = WriteCRLFHeader(headersInsertPtr, http::Connection, connectionValue);
  }

  _data.addSize(totalNewHeadersSize);

  WriteCRLFDateHeader(_data.data() + headersStartPos(), tp);
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
  assert(_trailerLen == 0);
  const auto bodySz = bodyLength();
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
  // This function is only called to set reserved headers Content-Length (which is never empty) and Content-Type
  // (which we never set to empty, it would be rejected by a std::invalid_argument).
  assert(!newValue.empty());
  Copy(newValue, first);
}

}  // namespace aeronet
