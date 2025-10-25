#include "aeronet/http-response.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "file.hpp"
#include "header-write.hpp"
#include "invalid_argument_exception.hpp"
#include "log.hpp"
#include "string-equal-ignore-case.hpp"
#include "stringconv.hpp"
#include "tchars.hpp"
#include "timedef.hpp"
#include "timestring.hpp"

namespace aeronet {

HttpResponse::HttpResponse(http::StatusCode code, std::string_view reason)
    : _data(kHttp1VersionLen + 1U + 3U + (reason.empty() ? 0UL : reason.size() + 1UL) + http::DoubleCRLF.size()),
      _bodyStartPos(static_cast<uint32_t>(_data.capacity())) {
  statusCode(code);
  if (!reason.empty()) {
    std::memcpy(_data.data() + kReasonBeg, reason.data(), reason.size());
  }
  _data.setSize(_data.capacity());
  std::memcpy(_data.data() + _data.size() - http::DoubleCRLF.size(), http::DoubleCRLF.data(), http::DoubleCRLF.size());
}

void HttpResponse::setReason(std::string_view newReason) {
  static constexpr std::size_t kMaxReasonLength = 1024;
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
    _data.ensureAvailableCapacity(static_cast<std::size_t>(diff));
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

void HttpResponse::setHeader(std::string_view newKey, std::string_view newValue, bool onlyIfNew) {
  assert(!newKey.empty() && std::ranges::all_of(newKey, [](char ch) { return is_tchar(ch); }));
  if (CaseInsensitiveEqual(newKey, http::ContentEncoding)) {
    _userProvidedContentEncoding = true;
  }
  if (_headersStartPos == 0) {
    appendHeaderUnchecked(newKey, newValue);
    return;
  }
  auto haystack = std::ranges::subrange(_data.data() + _headersStartPos + http::CRLF.size(),
                                        _data.data() + _bodyStartPos - http::CRLF.size());

  while (!haystack.empty()) {
    // At this point haystack is pointing to the beginning of a header, immediately after a CRLF.
    // For instance: 'Content-Encoding: gzip\r\n'
    // Per design, we enforce the header separator to be exactly http::HeaderSep
    auto nextHeaderSepRg = std::ranges::search(haystack, http::HeaderSep);
    if (nextHeaderSepRg.empty()) {
      throw std::runtime_error("Unable to locate the header separator");
    }
    std::string_view oldHeaderKey(haystack.begin(), nextHeaderSepRg.begin());

    // move haystack to beginning of old header value
    haystack = std::ranges::subrange(nextHeaderSepRg.end(), haystack.end());

    const auto nextCRLFRg = std::ranges::search(haystack, http::CRLF);
    if (nextCRLFRg.empty()) {
      throw std::runtime_error("Invalid header value");
    }

    // move haystack to next header
    haystack = std::ranges::subrange(nextCRLFRg.end(), haystack.end());

    if (!CaseInsensitiveEqual(oldHeaderKey, newKey)) {
      continue;
    }

    // Same header
    if (onlyIfNew) {
      return;
    }

    char* valueFirst = nextHeaderSepRg.begin() + http::HeaderSep.size();
    const std::size_t oldHeaderValueSz = static_cast<std::size_t>(nextCRLFRg.begin() - valueFirst);

    const auto diff = static_cast<int64_t>(newValue.size()) - static_cast<int64_t>(oldHeaderValueSz);
    if (diff == 0) {
      std::memcpy(valueFirst, newValue.data(), newValue.size());
      return;
    }
    const auto valuePos = static_cast<std::size_t>(valueFirst - _data.data());
    if (diff > 0) {
      _data.ensureAvailableCapacity(static_cast<std::size_t>(diff));
      valueFirst = _data.data() + valuePos;
    }
    std::memmove(valueFirst + newValue.size(), valueFirst + oldHeaderValueSz,
                 _data.size() - valuePos - oldHeaderValueSz);
    std::memcpy(valueFirst, newValue.data(), newValue.size());
    _data.addSize(static_cast<std::size_t>(diff));
    _bodyStartPos = static_cast<uint32_t>(static_cast<int64_t>(_bodyStartPos) + diff);
    return;
  }
  appendHeaderUnchecked(newKey, newValue);
}

void HttpResponse::setBodyInternal(std::string_view newBody) {
  if (_trailerPos != 0) {
    throw std::logic_error("Cannot set body after the first trailer");
  }
  const int64_t diff = static_cast<int64_t>(newBody.size()) - static_cast<int64_t>(internalBodyAndTrailersLen());
  if (diff > 0) {
    int64_t newBodyInternalPos = -1;
    if (newBody.data() > _data.data() && newBody.data() <= _data.data() + _data.size()) {
      // the memory pointed by newBody is internal to HttpResponse. We need to save the position before
      // realloc
      newBodyInternalPos = newBody.data() - _data.data();
    }
    _data.ensureAvailableCapacity(static_cast<std::size_t>(diff));
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
  _payloadKind = PayloadKind::Inline;
}

HttpResponse& HttpResponse::sendFile(File file, std::size_t offset, std::size_t length) & {
  if (!file) {
    throw invalid_argument("sendFile requires an opened file");
  }
  const std::size_t fileSize = file.size();
  if (offset > fileSize) {
    throw invalid_argument("sendFile offset exceeds file size");
  }
  const std::size_t resolvedLength = length == 0 ? (fileSize - offset) : length;
  if (offset + resolvedLength > fileSize) {
    throw invalid_argument("sendFile length exceeds file size");
  }
  if (_trailerPos != 0) {
    throw std::logic_error("Cannot call sendFile after adding trailers");
  }

  setBodyInternal(std::string_view());
  _payloadKind = PayloadKind::File;
  _payloadVariant.emplace<FilePayload>(std::move(file), offset, resolvedLength);
  return *this;
}

std::string_view HttpResponse::body() const noexcept {
  if (_payloadKind == PayloadKind::File) {
    return {};
  }
  const HttpPayload* pExternPayload = externPayloadPtr();
  auto ret =
      pExternPayload != nullptr ? pExternPayload->view() : std::string_view{_data.begin() + _bodyStartPos, _data.end()};
  if (_trailerPos != 0) {
    ret.remove_suffix(ret.size() - _trailerPos);
  }
  return ret;
}

void HttpResponse::appendHeaderUnchecked(std::string_view key, std::string_view value) {
  assert(!key.empty() && std::ranges::all_of(key, [](char ch) { return is_tchar(ch); }));
  if (CaseInsensitiveEqual(key, http::ContentEncoding)) {
    _userProvidedContentEncoding = true;
  }
  if (_headersStartPos == 0) {
    _headersStartPos = static_cast<decltype(_headersStartPos)>(_bodyStartPos - http::DoubleCRLF.size());
  }
  const std::size_t headerLineSize = http::CRLF.size() + key.size() + http::HeaderSep.size() + value.size();
  _data.ensureAvailableCapacity(headerLineSize);
  char* insertPtr = _data.data() + _bodyStartPos - http::DoubleCRLF.size();
  std::memmove(insertPtr + headerLineSize, insertPtr, http::DoubleCRLF.size() + internalBodyAndTrailersLen());

  WriteCRLFHeader(insertPtr, key, value);

  _data.addSize(headerLineSize);
  _bodyStartPos += static_cast<uint32_t>(headerLineSize);
}

void HttpResponse::appendDateUnchecked(SysTimePoint tp) {
  assert(_headersStartPos != 0);

  const std::size_t headerLineSize =
      http::CRLF.size() + http::Date.size() + http::HeaderSep.size() + kRFC7231DateStrLen;
  _data.ensureAvailableCapacity(headerLineSize);
  char* insertPtr = _data.data() + _bodyStartPos - http::DoubleCRLF.size();
  std::memmove(insertPtr + headerLineSize, insertPtr, http::DoubleCRLF.size() + internalBodyAndTrailersLen());

  WriteCRLFDateHeader(insertPtr, tp);

  _data.addSize(headerLineSize);
  _bodyStartPos += static_cast<uint32_t>(headerLineSize);
}

void HttpResponse::appendTrailer(std::string_view name, std::string_view value) {
  assert(!name.empty() && std::ranges::all_of(name, [](char ch) { return is_tchar(ch); }));
  if (_payloadKind == PayloadKind::File) {
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
    pExternPayload->ensureAvailableCapacity(lineSize + http::CRLF.size());
    insertPtr = pExternPayload->data() + pExternPayload->size();
    if (_trailerPos == 0) {
      // store trailer position relative to the start of the body (captured body case)
      _trailerPos = pExternPayload->size();
    }
    pExternPayload->addSize(lineSize);
  } else {
    _data.ensureAvailableCapacity(lineSize + http::CRLF.size());
    insertPtr = _data.data() + _data.size();
    if (_trailerPos == 0) {
      // _trailerPos is stored relative to the start of the body. For inline bodies the
      // body begins at _bodyStartPos so compute the relative offset here.
      _trailerPos = internalBodyAndTrailersLen();
    }
    _data.addSize(lineSize);
  }

  std::memcpy(insertPtr, name.data(), name.size());
  insertPtr += name.size();
  std::memcpy(insertPtr, http::HeaderSep.data(), http::HeaderSep.size());
  insertPtr += http::HeaderSep.size();
  if (!value.empty()) {
    std::memcpy(insertPtr, value.data(), value.size());
    insertPtr += value.size();
  }
  std::memcpy(insertPtr, http::CRLF.data(), http::CRLF.size());
}

HttpResponse::PreparedResponse HttpResponse::finalizeAndStealData(http::Version version, SysTimePoint tp,
                                                                  bool keepAlive,
                                                                  std::span<const http::Header> globalHeaders,
                                                                  bool isHeadMethod, std::size_t minCapturedBodySize) {
  const auto versionStr = version.str();
  std::memcpy(_data.data(), versionStr.data(), versionStr.size());
  _data[versionStr.size()] = ' ';
  // status code already set. Space before reason only if reason present.
  const auto rLen = reasonLen();
  if (rLen != 0) {
    _data[kReasonBeg - 1UL] = ' ';
  }

  const auto bodySz = bodyLen();
  if (bodySz != 0) {
    appendHeaderUnchecked(http::ContentLength, std::string_view(IntegralToCharVector(bodySz)));
  }
  if (isHeadMethod) {
    // erase body (but keep ContentLength)
    body(std::string_view());
  }

  appendHeaderUnchecked(http::Connection, keepAlive ? http::keepalive : http::close);
  appendDateUnchecked(tp);

  for (const auto& [headerKey, headerValue] : globalHeaders) {
    setHeader(headerKey, headerValue, true);
  }

  HttpPayload* pExternPayload = externPayloadPtr();
  if (bodySz <= minCapturedBodySize && pExternPayload != nullptr) {
    // move body into main buffer
    // bypass trailer check
    auto capturedTrailerPos = std::exchange(_trailerPos, 0);
    auto externPayloadView = pExternPayload->view();
    _data.reserve(_bodyStartPos + externPayloadView.size() + (capturedTrailerPos != 0 ? http::CRLF.size() : 0));
    setBodyInternal(externPayloadView);
    _trailerPos = capturedTrailerPos;
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
      // Ensure the captured payload has its own capacity/materialized buffer
      // (converts CharBuffer -> RawChars if needed) before appending so we
      // don't rely on potentially-invalid pointers into the main _data buffer.
      pExternPayload->append(http::CRLF);
    } else {
      // The extra CRLF size has already been added earlier.
      _data.unchecked_append(http::CRLF);
    }
  }

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

}  // namespace aeronet
