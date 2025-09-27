#include "aeronet/http-response.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include "http-constants.hpp"
#include "http-status-code.hpp"
#include "stringconv.hpp"

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
    _headersStartPos = static_cast<uint32_t>(static_cast<int64_t>(_headersStartPos) + diff);
  }
  std::memcpy(_data.data() + kReasonBeg, newReason.data(), newReason.size());
  _data.setSize(static_cast<std::size_t>(static_cast<int64_t>(_data.size()) + diff));
}

void HttpResponse::setHeader(std::string_view newKey, std::string_view newValue) {
  assert(!newKey.empty());
  if (_headersStartPos == 0) {
    appendHeaderUnchecked(newKey, newValue);
    return;
  }
  auto first = _data.data() + _headersStartPos;
  const auto last = _data.data() + _bodyStartPos;
  while (first < last) {
    first = std::search(first, last, newKey.data(), newKey.data() + newKey.size());
    if (first == last) {
      break;
    }
    if (std::memcmp(first - http::CRLF.size(), http::CRLF.data(), http::CRLF.size()) != 0) {
      ++first;
      continue;
    }
    first += newKey.size();
    if (first + http::HeaderSep.size() >= last) {
      break;
    }
    if (std::memcmp(first, http::HeaderSep.data(), http::HeaderSep.size()) != 0) {
      continue;
    }
    first += http::HeaderSep.size();
    const std::string_view newValueView(newValue);
    auto endOldValueIt = std::search(first, last, http::CRLF.data(), http::CRLF.data() + http::CRLF.size());
    if (endOldValueIt == last) {
      throw std::runtime_error("Invalid header value");
    }
    const std::string_view oldValueView(first, endOldValueIt);
    const int64_t diff = static_cast<int64_t>(newValueView.size()) - static_cast<int64_t>(oldValueView.size());
    if (diff == 0) {
      std::memcpy(first, newValueView.data(), newValueView.size());
      return;
    }
    const auto firstPos = static_cast<std::size_t>(first - _data.data());
    if (diff > 0) {
      _data.ensureAvailableCapacity(static_cast<std::size_t>(diff));
      first = _data.data() + firstPos;
    }
    std::memmove(first + newValueView.size(), first + oldValueView.size(),
                 _data.size() - firstPos - oldValueView.size());
    std::memcpy(first, newValueView.data(), newValueView.size());
    _data.setSize(static_cast<std::size_t>(static_cast<int64_t>(_data.size()) + diff));
    _bodyStartPos = static_cast<uint32_t>(static_cast<int64_t>(_bodyStartPos) + diff);
    return;
  }
  appendHeaderUnchecked(newKey, newValue);
}

void HttpResponse::setBody(std::string_view newBody) {
  const int64_t diff = static_cast<int64_t>(newBody.size()) - static_cast<int64_t>(body().size());
  if (diff > 0) {
    int64_t newBodyInternalPos = -1;
    if (newBody.data() > _data.data() && newBody.data() <= _data.data() + _data.size()) {
      // the memory pointed by newBody is internal to HttpResponse. We need to save the position before realloc
      newBodyInternalPos = newBody.data() - _data.data();
    }
    _data.ensureAvailableCapacity(static_cast<std::size_t>(diff));
    if (newBodyInternalPos != -1) {
      // restore the original data
      newBody = std::string_view(_data.data() + newBodyInternalPos, newBody.size());
    }
  }
  if (!newBody.empty()) {
    // Because calling memcpy with a null pointer is undefined behavior even if size is 0
    std::memcpy(_data.data() + _bodyStartPos, newBody.data(), newBody.size());
  }

  _data.setSize(static_cast<std::size_t>(static_cast<int64_t>(_data.size()) + diff));
}

void HttpResponse::appendHeaderUnchecked(std::string_view key, std::string_view value) {
  assert(!key.empty());
  // We model header insertion as: CRLF + key + ": " + value (NO trailing CRLF here).
  // The trailing CRLF for a header line is provided by the leading CRLF of the next header
  // OR by the first CRLF inside the final DoubleCRLF sentinel. This allows append-only
  // behavior without rewriting the tail for each header.
  const std::size_t headerLineSize = http::CRLF.size() + key.size() + http::HeaderSep.size() + value.size();
  _data.ensureAvailableCapacity(headerLineSize);

  auto insertPos = _data.data() + _bodyStartPos - http::DoubleCRLF.size();
  std::memmove(insertPos + headerLineSize, insertPos, http::DoubleCRLF.size() + bodyLen());

  // Leading CRLF terminates previous line (status line or previous header).
  std::memcpy(insertPos, http::CRLF.data(), http::CRLF.size());
  insertPos += http::CRLF.size();
  std::memcpy(insertPos, key.data(), key.size());
  insertPos += key.size();
  std::memcpy(insertPos, http::HeaderSep.data(), http::HeaderSep.size());
  insertPos += http::HeaderSep.size();
  std::memcpy(insertPos, value.data(), value.size());

  if (_headersStartPos == 0) {
    // First header key begins after inserted leading CRLF.
    _headersStartPos = static_cast<uint32_t>((_bodyStartPos - http::DoubleCRLF.size()) + http::CRLF.size());
  }
  _data.setSize(_data.size() + headerLineSize);
  _bodyStartPos += static_cast<uint32_t>(headerLineSize);
}

std::string_view HttpResponse::finalizeAndGetFullTextResponse(std::string_view version, std::string_view date,
                                                              bool keepAlive, bool isHeadMethod) {
  assert(version.size() == kHttp1VersionLen);  // "HTTP/x.y"
  std::memcpy(_data.data(), version.data(), kHttp1VersionLen);
  _data[kHttp1VersionLen] = ' ';
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

  appendHeaderUnchecked(http::Date, date);
  appendHeaderUnchecked(http::Connection, keepAlive ? http::keepalive : http::close);

  return _data;
}

}  // namespace aeronet