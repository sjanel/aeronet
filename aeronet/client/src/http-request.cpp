#include "aeronet/http-request.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-client-codec.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message-common.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/url-parsed-result.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/search-crlf.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "url-parse.hpp"

namespace aeronet {

namespace {

constexpr std::string_view kSchemeSep = "://";

constexpr bool IsHex(unsigned char ch) noexcept {
  return (ch >= '0' && ch <= '9') || ((ch | 0x20U) >= 'a' && (ch | 0x20U) <= 'f');
}

// Tells whether [pData, end) starts with the 2 hex digits ending a percent-escape.
constexpr bool StartsWithHexPair(const unsigned char* pData, const unsigned char* end) noexcept {
  return end - pData >= 2 && IsHex(pData[0]) && IsHex(pData[1]);
}

// Checks the chars of (a part of) a request-target, which may be empty.
// If 'afterPercent' is true, 'chars' directly follows a '%' and must start with the 2 hex digits completing its escape.
bool IsValidRequestTargetChars(std::string_view chars, bool afterPercent = false) noexcept {
  const auto* pData = reinterpret_cast<const unsigned char*>(chars.data());
  const auto* end = pData + chars.size();

  if (afterPercent) {
    if (!StartsWithHexPair(pData, end)) {
      return false;
    }
    pData += 2;
  }

  while (pData != end) {
    unsigned char ch = *pData++;

    // RFC 9112 :
    // request-target must not contain SP, CTL or DEL.

    if (ch <= 0x20 || ch == 0x7F) {
      return false;
    }

    if (ch == '%') {
      if (!StartsWithHexPair(pData, end)) {
        return false;
      }

      pData += 2;
    }
  }

  return true;
}

bool IsValidRequestTarget(std::string_view target) noexcept {
  return !target.empty() && IsValidRequestTargetChars(target);
}

// Maximum request-target length, so that the head up to the headers (origin key + request line) fits in
// headerPosNbBits bits.
constexpr uint32_t MaxTargetLen(uint8_t headerPosNbBits, uint32_t originKeyLen) {
  static constexpr std::uint32_t kMaxMethodStrLen = static_cast<uint32_t>(
      std::ranges::max_element(http::kMethodStrings, {}, [](std::string_view str) { return str.size(); })->size());

  return static_cast<uint32_t>((1U << headerPosNbBits) - 1U - kMaxMethodStrLen - 2U - http::HTTP10Sv.size() -
                               originKeyLen);
}

constexpr const char* CheckTarget(std::string_view target, uint8_t headerPosNbBits, uint32_t originKeyLen) {
  if (!IsValidRequestTarget(target)) {
    return "Invalid HTTP request target";
  }
  if (target.size() > MaxTargetLen(headerPosNbBits, originKeyLen)) {
    return "Request target exceeds maximum length";
  }
  return nullptr;
}

constexpr void CheckTargetOrThrow(std::string_view target, uint8_t headerPosNbBits, uint32_t originKeyLen) {
  if (const char* err = CheckTarget(target, headerPosNbBits, originKeyLen)) {
    throw std::invalid_argument(err);
  }
}

// Initial size of the HttpMessage internal buffer, including the status line and DoubleCRLF.
// GET / HTTP/1.0\r\n\r\n
constexpr std::size_t HttpRequestInitialSize(http::Method method, bool hasNonTlsProxy, uint8_t headerPosNbBits,
                                             uint32_t originKeyLen, std::string_view target) {
  CheckTargetOrThrow(target, headerPosNbBits, originKeyLen);
  std::size_t sz = http::MethodToStr(method).size() + 1U + target.size();
  if (hasNonTlsProxy) {
    // Absolute-form request-target for a cleartext proxy: "scheme://host:port" (the origin key) + origin-form.
    sz += originKeyLen;
  }
  return sz + 1U + http::HTTP10Sv.size() + http::DoubleCRLF.size();
}

inline char* AppendScheme(bool isTls, char* pData) {
  if (isTls) {
    static constexpr std::string_view kHttps = "https://";
    pData = AppendFixed<kHttps>(pData);
  } else {
    static constexpr std::string_view kHttp = "http://";
    pData = AppendFixed<kHttp>(pData);
  }
  return pData;
}

constexpr char* InitData(http::Method method, bool hasNonTlsProxy, bool hostIsIpv6,
                         const internal::UrlParseResult& urlParseResult, char* pData) {
  // Write origin key at beginning of buffer: "scheme://host:port" (RFC 9112 section 3.2.1, 3.2.2, 9.3.6).
  char* const pOriginKeyBeg = pData;
  pData = AppendScheme(urlParseResult.isTls, pData);
  pData = Append(urlParseResult.host, pData);
  *pData++ = ':';  // port is always specified in origin key

  const auto portNbDigits = ndigits(urlParseResult.port);
  pData = WriteUInt(pData, urlParseResult.port, portNbDigits);
  const std::string_view originKey(pOriginKeyBeg, pData);

  // From there, the request buffer will start.
  pData = Append(http::MethodToStr(method), pData);
  *pData++ = ' ';

  if (hasNonTlsProxy) {
    // For a cleartext request sent to a forward proxy, the request-target is the absolute-form URL (RFC 9112 section
    // 3.2.2), spelled with an explicit (normalized) port: the origin key ("scheme://host:port") followed by the
    // origin-form target (path + optional "?query").
    pData = Append(originKey, pData);
    pData = Append(urlParseResult.target, pData);
  } else {
    // For a direct request to the origin, the request-target is the origin-form (path + optional "?query") (RFC 9112
    // section 3.2.1).
    pData = Append(urlParseResult.target, pData);
  }

  *pData++ = ' ';

  static constexpr std::string_view kHostPrefix =
      JoinStringView_v<http::HTTP11Sv, http::CRLF, http::Host, http::HeaderSep>;

  // Host Header
  pData = AppendFixed<kHostPrefix>(pData);

  if (hostIsIpv6) {
    *pData++ = '[';
  }
  pData = Append(urlParseResult.host, pData);
  if (hostIsIpv6) {
    *pData++ = ']';
  }
  if (urlParseResult.hasNonDefaultPort()) {
    *pData++ = ':';
    pData = WriteUInt(pData, urlParseResult.port, portNbDigits);
  }

  return pData;
}

std::size_t ComputeHostHeaderSize(std::string_view host, bool hostIsIpv6, bool hasNonDefaultPort,
                                  std::uint8_t portNbDigits) {
  std::size_t sz = http::HeaderSize(http::Host.size(), host.size());
  if (hostIsIpv6) {
    sz += 2U;  // '[' + ']'
  }
  if (hasNonDefaultPort) {
    sz += 1U + portNbDigits;  // ':' + port digits
  }
  return sz;
}

}  // namespace

HttpRequest::HttpRequest(std::size_t additionalCapacity, http::Method method, std::string_view url,
                         std::string_view concatenatedHeaders, Options opts)
    : HttpMessage(opts) {
  const auto res = internal::ParseUrl(url);
  if (res.host.empty()) {
    throw std::invalid_argument("Invalid URL");
  }

  const auto portNbDigits = ndigits(res.port);
  const bool hasNonTlsProxy = opts.hasProxy() && !res.isTls;
  const auto schemeLen = static_cast<uint8_t>((res.isTls ? internal::kHttps : internal::kHttp).size());

  _hostLen = SafeCast<decltype(_hostLen)>(res.host.size());
  _port = res.port;
  _originKeyLen = SafeCast<decltype(_originKeyLen)>(schemeLen + kSchemeSep.size() + _hostLen + 1U + portNbDigits);

  const bool hostIsIpv6 = res.host.contains(':');
  auto hostHeaderSize = ComputeHostHeaderSize(res.host, hostIsIpv6, res.hasNonDefaultPort(), portNbDigits);

  const auto neededCapacity =
      HttpRequestInitialSize(method, hasNonTlsProxy, HttpMessage::kHeaderPosNbBits, _originKeyLen, res.target) +
      hostHeaderSize + concatenatedHeaders.size() + additionalCapacity + _originKeyLen;

  _data.reserve(neededCapacity);

  char* pData = InitData(method, hasNonTlsProxy, hostIsIpv6, res, _data.data());
  setHeadersStartPosNoCheck(static_cast<uint64_t>(pData - _data.data()) - hostHeaderSize);
  if (concatenatedHeaders.empty()) {
    pData = AppendFixed<http::DoubleCRLF>(pData);
  } else {
    pData = AppendFixed<http::CRLF>(pData);
    pData = Append(concatenatedHeaders, pData);
    pData = AppendFixed<http::CRLF>(pData);
  }

  _data.setEnd(pData);

  // Derive the body start from the head actually written, not from neededCapacity: the latter also covers
  // additionalCapacity, which is reserved but deliberately left unwritten for the caller's own headers. Setting it from
  // the capacity would leave bodyStartPos additionalCapacity bytes past the data, and every later header mutation
  // derives its insertion pointer from it (see HttpMessage::headerAddLineUnchecked).
  setBodyStartPos(_data.size());

  assert(_data.size() + additionalCapacity == _data.capacity());
}

HttpRequest::HttpRequest(std::size_t additionalCapacity, http::Method method, std::string_view url,
                         std::string_view concatenatedHeaders, Options opts, std::string_view body,
                         std::string_view contentType)
    : HttpMessage(opts) {
  const auto res = internal::ParseUrl(url);

  if (res.host.empty()) {
    throw std::invalid_argument("Invalid URL");
  }

  const auto portNbDigits = ndigits(res.port);
  const bool hasNonTlsProxy = opts.hasProxy() && !res.isTls;
  const auto schemeLen = static_cast<uint8_t>((res.isTls ? internal::kHttps : internal::kHttp).size());

  _hostLen = SafeCast<decltype(_hostLen)>(res.host.size());
  _port = res.port;
  _originKeyLen = SafeCast<decltype(_originKeyLen)>(schemeLen + kSchemeSep.size() + _hostLen + 1U + portNbDigits);

  const bool hostIsIpv6 = res.host.contains(':');
  const auto hostHeaderSize = ComputeHostHeaderSize(res.host, hostIsIpv6, res.hasNonDefaultPort(), portNbDigits);

  const auto neededCapacity =
      HttpRequestInitialSize(method, hasNonTlsProxy, HttpMessage::kHeaderPosNbBits, _originKeyLen, res.target) +
      hostHeaderSize + concatenatedHeaders.size() +
      NeededBodyHeadersSize(body.size(), CheckContentType(body.empty(), contentType).size()) + body.size() +
      additionalCapacity + _originKeyLen;

  _data.reserve(neededCapacity);

  char* pData = InitData(method, hasNonTlsProxy, hostIsIpv6, res, _data.data());
  setHeadersStartPosNoCheck(static_cast<uint64_t>(pData - _data.data()) - hostHeaderSize);
  if (!concatenatedHeaders.empty()) {
    pData = AppendFixed<http::CRLF>(pData);
    pData = Append(concatenatedHeaders, pData);
    pData -= http::CRLF.size();  // remove the last CRLF
  }
  if (!body.empty()) {
    pData = AppendFixed<http::CRLF>(pData);

    pData = WriteContentTypeContentLengthDoubleCRLF(contentType, body.size(), pData);

    pData = Append(body, pData);
  } else {
    pData = AppendFixed<http::DoubleCRLF>(pData);
  }
  _data.setEnd(pData);
  assert(_data.size() + additionalCapacity == _data.capacity());
  setBodyStartPos(_data.size() - body.size());
}

HttpRequest& HttpRequest::method(http::Method method) & {
  const http::Method oldMethod = this->method();
  if (method == oldMethod) {
    return *this;
  }

  const auto newMethodStr = http::MethodToStr(method);
  const auto oldMethodLen = http::MethodToStr(oldMethod).size();
  const auto newMethodLen = newMethodStr.size();
  const int32_t diffLen = static_cast<int32_t>(newMethodLen) - static_cast<int32_t>(oldMethodLen);

  HeadGrowthManager headGrowthManager(*this, diffLen);

  // Shift the [method-end, end) tail by `diffLen` to make room for the new method string.
  assert(_data.size() >= oldMethodLen + _originKeyLen);
  std::memmove(_data.data() + newMethodLen + _originKeyLen, _data.data() + oldMethodLen + _originKeyLen,
               _data.size() - oldMethodLen - _originKeyLen);

  // Adjust positions and size
  adjustHeadersAndBodyStart(diffLen);

  _data.adjustSize(diffLen);

  Copy(newMethodStr, _data.data() + _originKeyLen);

  return *this;
}

HttpRequest& HttpRequest::target(std::string_view target) & {
  CheckTargetOrThrow(target, HttpMessage::kHeaderPosNbBits, _originKeyLen);

  const auto oldTarget = this->target();

  const auto oldTargetLen = oldTarget.size();
  const auto newTargetLen = target.size();

  const int32_t diffLen = static_cast<int32_t>(newTargetLen) - static_cast<int32_t>(oldTargetLen);

  HeadGrowthManager headGrowthManager(*this, diffLen);

  char* pData = _data.data();

  const uint32_t offset = _originKeyLen + methodLen() + 1U;  // after "<METHOD> "

  // Move everything after the URI (starting with the space before HTTP/x.x).
  std::memmove(pData + offset + newTargetLen, pData + offset + oldTargetLen, _data.size() - (offset + oldTargetLen));

  Copy(target, pData + offset);

  adjustHeadersAndBodyStart(diffLen);

  _data.adjustSize(diffLen);

  return *this;
}

const char* HttpRequest::setNewUrl(const internal::UrlParseResult& res) {
  const auto portNbDigits = ndigits(res.port);
  const auto schemeLen = static_cast<uint8_t>((res.isTls ? internal::kHttps : internal::kHttp).size());
  const auto hostLen = SafeCast<decltype(_hostLen)>(res.host.size());
  const auto originKeyLen =
      SafeCast<decltype(_originKeyLen)>(schemeLen + kSchemeSep.size() + hostLen + 1U + portNbDigits);

  // Validate before modifying anything, so that a rejected URL leaves this request unchanged.
  const char* pErrorMsg = CheckTarget(res.target, HttpMessage::kHeaderPosNbBits, originKeyLen);
  if (pErrorMsg != nullptr) {
    return pErrorMsg;
  }

  const auto* pHostHeaderEnd = SearchCRLF(
      _data.data() + http::CRLF.size() + headersStartPos() + http::Host.size() + http::HeaderSep.size(), _data.end());
  assert(pHostHeaderEnd != _data.end() && pHostHeaderEnd[1] == '\n');
  const auto oldHostHeaderEndPos = static_cast<uint64_t>(pHostHeaderEnd - _data.data());

  const bool hasNonTlsProxy = _opts.hasProxy() && !res.isTls;
  // Read from the buffer with the old origin key length.
  const auto method = this->method();
  const auto methodLen = this->methodLen();

  _hostLen = hostLen;
  _port = res.port;
  _originKeyLen = originKeyLen;

  const bool hostIsIpv6 = res.host.contains(':');
  const auto hostHeaderSize = ComputeHostHeaderSize(res.host, hostIsIpv6, res.hasNonDefaultPort(), portNbDigits);

  const auto newHostHeaderEndPos =
      _originKeyLen + methodLen + 1U + res.target.size() + 1U + http::HTTP11Sv.size() + hostHeaderSize;
  const int32_t diffLen = static_cast<int32_t>(newHostHeaderEndPos) - static_cast<int32_t>(oldHostHeaderEndPos);

  HeadGrowthManager headGrowthManager(*this, diffLen);

  char* pData = _data.data();

  // Move everything after the origin key.
  std::memmove(pData + newHostHeaderEndPos, pData + oldHostHeaderEndPos, _data.size() - oldHostHeaderEndPos);

  char* pInsert = InitData(method, hasNonTlsProxy, hostIsIpv6, res, pData);

  setHeadersStartPosNoCheck(static_cast<uint64_t>(pInsert - pData) - hostHeaderSize);
  adjustBodyStart(diffLen);
  _data.adjustSize(diffLen);
  return nullptr;
}

HttpRequest::RedirectOutcome HttpRequest::resolveRedirect(std::string_view location) {
  // Absolute URL, or network-path reference '//host[:port][/path]' that inherits the scheme. The latter's authority is
  // parsed directly to build the canonical buffer once, instead of synthesizing a "scheme://..." string to re-parse.
  const bool isAbsoluteUrl = location.contains("://");
  if (isAbsoluteUrl || location.starts_with("//")) {
    internal::UrlParseResult res;
    if (isAbsoluteUrl) {
      res = internal::ParseUrl(location);
    } else {
      res.isTls = isTlsRequest();
      internal::ParseAuthority(location.substr(2), res);
    }
    if (res.host.empty()) {
      return RedirectOutcome::Invalid;
    }
    if (isTlsRequest() && !res.isTls) {
      return RedirectOutcome::Downgrade;
    }
    const bool sameOrigin = res.isTls == isTlsRequest() && res.port == port() && CaseInsensitiveEqual(res.host, host());
    if (setNewUrl(res) != nullptr) {
      return RedirectOutcome::Invalid;
    }
    return sameOrigin ? RedirectOutcome::SameOrigin : RedirectOutcome::CrossOrigin;
  }

  // Strip fragment from the relative reference.
  if (const auto hashPos = location.find('#'); hashPos != std::string_view::npos) {
    location = location.substr(0, hashPos);
  }

  const std::string_view oldTarget = target();
  const auto oldTargetLen = oldTarget.size();

  // Number of leading chars of the old target kept in the new one, followed by 'location'.
  uint32_t prefixLen;

  if (location.starts_with('/')) {
    // Absolute path: keep origin, replace target.
    prefixLen = 0;
  } else if (location.starts_with('?')) {
    // Keep the whole path and append the new query.
    prefixLen = static_cast<uint32_t>(oldTargetLen);
  } else {
    // Keep only the directory (including the trailing '/').
    std::string_view base = oldTarget;
    if (const auto questionMarkPos = base.find('?'); questionMarkPos != std::string_view::npos) {
      base = base.substr(0, questionMarkPos);
    }
    const auto lastSlashPos = base.rfind('/');
    prefixLen = lastSlashPos == std::string_view::npos ? 1U : static_cast<uint32_t>(lastSlashPos + 1);
  }

  const auto newTargetLen = prefixLen + location.size();
  assert(newTargetLen != 0);

  // Validate the new target before touching the buffer, so that a rejected redirect leaves this request unchanged.
  // The kept prefix comes from the old target, which is valid: it can only end inside a percent-escape when it is
  // reduced to its first char, a '%' whose 2 hex digits must then start 'location'.
  const bool prefixEndsWithPercent = prefixLen != 0 && oldTarget[prefixLen - 1] == '%';
  if (!IsValidRequestTargetChars(location, prefixEndsWithPercent) ||
      newTargetLen > MaxTargetLen(HttpMessage::kHeaderPosNbBits, _originKeyLen)) {
    return RedirectOutcome::Invalid;
  }

  const int32_t diffLen = static_cast<int32_t>(newTargetLen) - static_cast<int32_t>(oldTargetLen);

  HeadGrowthManager headGrowthManager(*this, diffLen);

  char* pTarget = targetBeg();

  // Move everything after the target (" HTTP/1.1"...).
  std::memmove(pTarget + newTargetLen, pTarget + oldTargetLen,
               _data.size() - oldTargetLen - static_cast<std::size_t>(pTarget - _data.data()));

  // Write 'location' after the kept prefix.
  Copy(location, pTarget + prefixLen);

  adjustHeadersAndBodyStart(diffLen);
  _data.adjustSize(diffLen);

  return RedirectOutcome::SameOrigin;
}

// Finalizes the HttpRequest and returns an HttpMessageData object that can be sent over the network.
// After calling this function, the HttpRequest object is still valid and can be reused to build another request,
// but the HttpMessageData has been created by copy. To avoid the copy, use the rvalue overload below.
[[nodiscard]] HttpRequest HttpRequest::finalizeHeadersAndBody(
    [[maybe_unused]] internal::HttpClientCodec& clientCodec,
    [[maybe_unused]] const DecompressionConfig& decompressionConfig) const {
  HttpRequest copy = *this;

  copy.HttpMessage::finalizeHeadersAndBody();

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  if (_opts.isAutomaticDirectCompression() && copy.trailersSize() == 0) {
    // We need to restore current HttpRequest compression state to make it as if finalize() was never called, so that
    // the current HttpRequest can be reused. This is because the compression state is shared between the current
    // HttpRequest and the copy, and finalizeInlineBody() modifies the compression state.
    auto& compressionState = *_opts._pCompressionState;

    // Then, we need to decompress the compressed data in the copy and restore the encoder state to what it was before
    // finalizeInlineBody() was called
    std::string_view decoded;
    const auto decodeRes = HttpCodec::DecompressFullBody(clientCodec.decompressionState, decompressionConfig,
                                                         headerValueOrEmpty(http::ContentEncoding), copy.bodyInMemory(),
                                                         clientCodec.decompressOut, clientCodec.decompressTmp, decoded);
    if (decodeRes.status != http::StatusCodeOK) {
      // Should not happen, it would either be a logic bug in the HTTP client code or a bad allocation.
      throw std::runtime_error("Failed to decompress body during finalize() of HttpRequest");
    }

    // Finally simulate the encode of the decoded data to restore the encoder state to what it was before
    // finalizeInlineBody() was called.
    auto& newContext = *compressionState.makeContext(_opts._pickedEncoding);
    const auto bodyStartPos = this->bodyStartPos();
    const auto result =
        newContext.encodeChunk(decoded, _data.size() - bodyStartPos, const_cast<char*>(_data.data() + bodyStartPos));
    if (result.hasError()) {
      throw std::runtime_error("Failed to restore encoder state during finalize() of HttpRequest");
    }
    const_cast<HttpRequest&>(*this)._data.setSize(bodyStartPos + result.writtenIfNoError());
  }
#endif

  return copy;
}

}  // namespace aeronet
