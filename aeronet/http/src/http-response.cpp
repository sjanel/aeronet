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

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "log.hpp"
#include "string-equal-ignore-case.hpp"
#include "stringconv.hpp"
#include "tchars.hpp"
#include "timedef.hpp"
#include "timestring.hpp"

namespace aeronet {

// (Helper removed: generic header insertion logic lives in HttpResponse::appendHeaderGeneric now.)

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
    // At this point haystack is pointing to the beginning of a header, immediately after a CRLF. For instance:
    // 'Content-Encoding: gzip\r\n'
    // Per design, we enforce the header separator to be exactly http::HeaderSep
    auto nextHeaderSepRg = std::ranges::search(haystack, http::HeaderSep);
    if (nextHeaderSepRg.empty()) {
      throw std::runtime_error("Unable to locate the header separator");
    }
    std::string_view oldHeaderKey(haystack.begin(), nextHeaderSepRg.begin());

    // move haystack to beginning of old header value
    haystack = std::ranges::subrange(nextHeaderSepRg.end(), haystack.end());

    auto nextCRLFRg = std::ranges::search(haystack, http::CRLF);
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

  _data.addSize(static_cast<std::size_t>(diff));
}

void HttpResponse::appendHeaderUnchecked(std::string_view key, std::string_view value) {
  assert(!key.empty() && std::ranges::all_of(key, [](char ch) { return is_tchar(ch); }));
  const bool markCE = CaseInsensitiveEqual(key, http::ContentEncoding);
  appendHeaderGeneric(
      key, value.size(),
      [value](char* dst) {
        if (!value.empty()) {
          std::memcpy(dst, value.data(), value.size());
        }
      },
      markCE);
}

void HttpResponse::appendDateUnchecked(TimePoint tp) {
  appendHeaderGeneric(http::Date, kRFC7231DateStrLen, [&](char* dst) { TimeToStringRFC7231(tp, dst); }, false);
}

std::string_view HttpResponse::finalizeAndGetFullTextResponse(http::Version version, TimePoint tp, bool keepAlive,
                                                              std::span<const http::Header> globalHeaders,
                                                              bool isHeadMethod) {
  auto versionStr = version.str();
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

  return _data;
}

}  // namespace aeronet
