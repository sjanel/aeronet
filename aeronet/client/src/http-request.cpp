#include "aeronet/http-request.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/decimal-writer.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/embedded-payload.hpp"
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
#include "aeronet/memory-utils.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/search-crlf.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/url-char-class.hpp"
#include "url-parse.hpp"

namespace aeronet {

namespace {

constexpr std::string_view kSchemeSep = "://";

// Tells whether [pData, pEnd) starts with the 2 hex digits ending a percent-escape.
bool StartsWithHexPair(const char* pData, const char* pEnd) noexcept {
  return pEnd - pData >= 2 && IsHexDigit(pData[0]) && IsHexDigit(pData[1]);
}

// Checks the chars of (a part of) a request-target, which may be empty.
bool IsValidRequestTargetChars(std::string_view chars) noexcept {
  for (const char *pData = chars.data(), *pEnd = pData + chars.size(); pData != pEnd;) {
    const char ch = *pData++;

    // RFC 9112 §3.2: a request-target contains no SP and no CTL (DEL included).
    if (ch == ' ' || url::IsControlChar(ch)) {
      return false;
    }

    if (ch == '%') {
      if (!StartsWithHexPair(pData, pEnd)) {
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

// Whether 'host', parsed by ParseAuthority(), can be written in the Host header and in a request-target: an IPv6
// address (hex digits, ':' and the '.' of an embedded IPv4 address), or a reg-name or an IPv4 address made of
// unreserved characters, sub-delims and percent-escapes (RFC 3986 §3.2.2). A space or a control character, which would
// split the head of the request, never is.
bool IsValidHost(std::string_view host) noexcept {
  if (host.contains(':')) {
    return std::ranges::all_of(host, [](char ch) { return IsHexDigit(ch) || ch == ':' || ch == '.'; });
  }
  for (const char *pData = host.data(), *pEnd = pData + host.size(); pData != pEnd;) {
    const char ch = *pData++;
    if (ch == '%') {
      if (!StartsWithHexPair(pData, pEnd)) {
        return false;
      }
      pData += 2;
    } else if (!url::IsRegNameChar(ch)) {
      return false;
    }
  }
  return true;
}

// Maximum length of the origin-form of a request-target, so that the head up to the headers fits in headerPosNbBits
// bits: the origin key, then the request line, whose target is prefixed by the origin key again in absolute-form (to a
// cleartext forward proxy).
constexpr uint32_t MaxTargetLen(uint8_t headerPosNbBits, uint32_t originKeyLen) {
  static constexpr std::uint32_t kMaxMethodStrLen = static_cast<uint32_t>(
      std::ranges::max_element(http::kMethodStrings, {}, [](std::string_view str) { return str.size(); })->size());

  return static_cast<uint32_t>((1U << headerPosNbBits) - 1U - kMaxMethodStrLen - 2U - http::HTTP10Sv.size() -
                               (std::size_t{2} * originKeyLen));
}

// The size of the request-target of the parsed 'target': the path of a URL without one is "/" (RFC 9112 section 3.2.1),
// so that a target parsed as "?query" is written "/?query".
constexpr std::size_t TargetSize(std::string_view target) noexcept {
  return target.size() + static_cast<std::size_t>(target.starts_with('?'));
}

constexpr const char* CheckTarget(std::string_view target, uint8_t headerPosNbBits, uint32_t originKeyLen) {
  if (!IsValidRequestTarget(target)) {
    return "Invalid HTTP request target";
  }
  if (TargetSize(target) > MaxTargetLen(headerPosNbBits, originKeyLen)) {
    return "Request target exceeds maximum length";
  }
  return nullptr;
}

constexpr void CheckTargetOrThrow(std::string_view target, uint8_t headerPosNbBits, uint32_t originKeyLen) {
  if (const char* err = CheckTarget(target, headerPosNbBits, originKeyLen)) {
    throw std::invalid_argument(err);
  }
}

// Length of the origin key "scheme://host:port" of a parsed URL (see InitData()).
constexpr uint32_t OriginKeyLen(const internal::UrlParseResult& res) noexcept {
  return static_cast<uint32_t>((res.isTls ? internal::kHttps : internal::kHttp).size() + kSchemeSep.size() +
                               res.host.size() + 1U + ndigits(res.port));
}

// The reason why the parsed URL 'res' cannot be requested, nullptr if it can: the checks of HttpRequest::IsValidUrl().
constexpr const char* CheckUrl(const internal::UrlParseResult& res, uint8_t headerPosNbBits) noexcept {
  if (res.host.empty()) {
    return "Invalid URL";
  }
  if (res.host.size() > std::numeric_limits<uint16_t>::max()) {
    return "URL host exceeds maximum length";
  }
  if (!IsValidHost(res.host)) {
    return "Invalid URL host";
  }
  return CheckTarget(res.target, headerPosNbBits, OriginKeyLen(res));
}

// Initial size of the HttpMessage internal buffer, including the status line and DoubleCRLF.
// GET / HTTP/1.0\r\n\r\n
constexpr std::size_t HttpRequestInitialSize(http::Method method, bool hasNonTlsProxy, uint32_t originKeyLen,
                                             std::string_view target) {
  std::size_t sz = http::MethodToStr(method).size() + 1U + TargetSize(target);
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
    // origin-form target.
    pData = Append(originKey, pData);
  }
  // For a direct request to the origin, the request-target is the origin-form (path + optional "?query") (RFC 9112
  // section 3.2.1), "/" for an empty path (see TargetSize()).
  if (urlParseResult.target.starts_with('?')) {
    *pData++ = '/';
  }
  pData = Append(urlParseResult.target, pData);

  *pData++ = ' ';

  // Host Header
  pData = AppendFixed<JoinStringView_v<http::HTTP11Sv, http::CRLF, http::Host, http::HeaderSep>>(pData);

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

void HttpRequest::initHead(std::size_t extraCapacity, http::Method method, std::string_view url,
                           std::string_view concatenatedHeaders, std::size_t bodySize, std::string_view contentType) {
  const auto res = internal::ParseUrl(url);
  if (const char* pErrorMsg = CheckUrl(res, HttpMessage::kHeaderPosNbBits)) {
    throw std::invalid_argument(pErrorMsg);
  }

  const auto portNbDigits = ndigits(res.port);
  const bool hasNonTlsProxy = _opts.hasProxy() && !res.isTls;

  _hostLen = static_cast<decltype(_hostLen)>(res.host.size());  // checked by CheckUrl()
  _port = res.port;
  _originKeyLen = OriginKeyLen(res);

  const bool hostIsIpv6 = res.host.contains(':');
  const auto hostHeaderSize = ComputeHostHeaderSize(res.host, hostIsIpv6, res.hasNonDefaultPort(), portNbDigits);

  const auto neededCapacity =
      HttpRequestInitialSize(method, hasNonTlsProxy, _originKeyLen, res.target) + hostHeaderSize +
      concatenatedHeaders.size() +
      (bodySize == 0 ? 0 : NeededBodyHeadersSize(bodySize, CheckContentType(contentType).size())) + extraCapacity +
      _originKeyLen;

  _data.reserve(neededCapacity);

  char* const pFirst = _data.data();
  char* pData = InitData(method, hasNonTlsProxy, hostIsIpv6, res, pFirst);
  const auto headersStartPos = static_cast<uint64_t>(pData - pFirst) - hostHeaderSize;
  if (!concatenatedHeaders.empty()) {
    pData = AppendFixed<http::CRLF>(pData);
    pData = Append(concatenatedHeaders, pData);
    pData -= http::CRLF.size();  // the last header CRLF is written below with the end of the head
  }
  if (bodySize == 0) {
    pData = AppendFixed<http::DoubleCRLF>(pData);
  } else {
    pData = AppendFixed<http::CRLF>(pData);
    pData = WriteContentTypeContentLengthDoubleCRLF(contentType, bodySize, pData);
  }

  // The body start is derived from the head actually written, not from neededCapacity: the latter also covers
  // extraCapacity, which is reserved but left unwritten (body, caller's own headers). Setting it from the capacity
  // would leave bodyStartPos past the data, and every later header mutation derives its insertion pointer from it (see
  // HttpMessage::headerAddLineUnchecked).
  setHeadPositions(headersStartPos, static_cast<uint64_t>(pData - pFirst));
}

bool HttpRequest::IsValidUrl(std::string_view url) noexcept {
  return CheckUrl(internal::ParseUrl(url), HttpMessage::kHeaderPosNbBits) == nullptr;
}

HttpRequest::HttpRequest(std::size_t additionalCapacity, http::Method method, std::string_view url,
                         std::string_view concatenatedHeaders, Options opts, HttpPayload body,
                         std::string_view contentType)
    : HttpMessage(opts) {
  const std::size_t bodySize = body.size();
  initHead(EmbeddedPayload::kMaxFootprint + additionalCapacity, method, url, concatenatedHeaders, bodySize,
           contentType);
  if (bodySize != 0) {
    setCapturedPayload(std::move(body));
  }
  assert(_data.size() + additionalCapacity <= _data.capacity());
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
  static constexpr std::string_view kAsterisk = "*";
  const bool absoluteForm = hasAbsoluteFormTarget();
  // The origin-form part of 'target', or "*". The absolute-form of the origin of this request that target() returns is
  // accepted too, the origin key being followed by an origin-form part or by nothing, for "*" (RFC 9112 §3.2.4).
  std::string_view originForm = target;
  if (absoluteForm && target.starts_with(originKey()) &&
      (target.size() == _originKeyLen || target[_originKeyLen] == '/' || target[_originKeyLen] == '?')) {
    originForm = target.size() == _originKeyLen ? kAsterisk : target.substr(_originKeyLen);
  }
  const bool asterisk = originForm == kAsterisk;
  if (!asterisk && !originForm.starts_with('/') && !originForm.starts_with('?')) {
    // An empty target, an absolute-form of another origin, or the target of another method (CONNECT).
    throw std::invalid_argument("Invalid HTTP request target");
  }
  CheckTargetOrThrow(originForm, HttpMessage::kHeaderPosNbBits, _originKeyLen);

  // The path of a URL without one is "/" (see TargetSize()), and the asterisk-form is an absolute-form without path.
  const bool addSlash = originForm.starts_with('?');
  if (asterisk && absoluteForm) {
    originForm = {};
  }
  const std::size_t newTargetLen = (absoluteForm ? _originKeyLen : 0U) + (addSlash ? 1U : 0U) + originForm.size();
  const std::size_t oldTargetLen = this->target().size();

  const int64_t diffLen = static_cast<int64_t>(newTargetLen) - static_cast<int64_t>(oldTargetLen);

  HeadGrowthManager headGrowthManager(*this, diffLen);

  char* pData = _data.data();

  const uint32_t offset = _originKeyLen + methodLen() + 1U;  // after "<METHOD> "

  // Move everything after the URI (starting with the space before HTTP/x.x).
  std::memmove(pData + offset + newTargetLen, pData + offset + oldTargetLen, _data.size() - (offset + oldTargetLen));

  char* pTarget = pData + offset;
  if (absoluteForm) {
    pTarget = Append(pData, _originKeyLen, pTarget);  // the origin key, at the start of the buffer
  }
  if (addSlash) {
    *pTarget++ = '/';
  }
  Copy(originForm, pTarget);

  adjustHeadersAndBodyStart(diffLen);

  _data.adjustSize(diffLen);

  return *this;
}

const char* HttpRequest::setNewUrl(const internal::UrlParseResult& res) {
  // Validate before modifying anything, so that a rejected URL leaves this request unchanged.
  if (const char* pErrorMsg = CheckUrl(res, HttpMessage::kHeaderPosNbBits)) {
    return pErrorMsg;
  }
  const auto portNbDigits = ndigits(res.port);
  const auto hostLen = static_cast<decltype(_hostLen)>(res.host.size());  // checked by CheckUrl()
  const uint32_t originKeyLen = OriginKeyLen(res);

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

  // Origin key, "<METHOD> ", target (prefixed with the origin key again in absolute-form for a cleartext proxy, see
  // InitData()), " HTTP/1.1" and the Host header line.
  const std::size_t newHostHeaderEndPos = _originKeyLen + methodLen + 1U + (hasNonTlsProxy ? _originKeyLen : 0U) +
                                          TargetSize(res.target) + 1U + http::HTTP11Sv.size() + hostHeaderSize;
  const int64_t diffLen = static_cast<int64_t>(newHostHeaderEndPos) - static_cast<int64_t>(oldHostHeaderEndPos);

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
    RedirectOutcome outcome = RedirectOutcome::CrossSite;
    if (res.isTls == isTlsRequest() && CaseInsensitiveEqual(res.host, host())) {
      outcome = res.port == port() ? RedirectOutcome::SameOrigin : RedirectOutcome::SameSite;
    }
    if (setNewUrl(res) != nullptr) {
      return RedirectOutcome::Invalid;
    }
    return outcome;
  }

  // Strip fragment from the relative reference.
  if (const auto hashPos = location.find('#'); hashPos != std::string_view::npos) {
    location = location.substr(0, hashPos);
  }

  if (location.empty()) {
    // A reference to the same document (RFC 3986 section 5.2.2): the target is unchanged.
    return RedirectOutcome::SameOrigin;
  }

  const std::string_view oldTarget = target();
  const std::size_t oldTargetLen = oldTarget.size();
  // An absolute-form target (to a cleartext proxy) keeps its origin key, followed by the origin-form part.
  const std::size_t originFormPos = hasAbsoluteFormTarget() ? _originKeyLen : 0U;
  const std::string_view originForm = oldTarget.substr(originFormPos);

  // Number of leading chars of the old target kept in the new one, followed by 'location', after a '/' when 'addSlash'.
  std::size_t prefixLen = originFormPos;
  bool addSlash = false;
  if (!originForm.starts_with('/')) {
    // The asterisk-form has no path: a relative reference is resolved against "/", as a URL with an authority and an
    // empty path is (RFC 3986 section 5.2.3).
    addSlash = !location.starts_with('/');
  } else if (location.starts_with('?')) {
    // Keep the path, replace the query (RFC 3986 section 5.2.2).
    prefixLen += std::min(originForm.find('?'), originForm.size());
  } else if (!location.starts_with('/')) {
    // Keep only the directory of the path (including its last '/').
    prefixLen += originForm.substr(0, originForm.find('?')).rfind('/') + 1U;
  }

  const std::size_t newTargetLen = prefixLen + (addSlash ? 1U : 0U) + location.size();

  // Validate the new target before touching the buffer, so that a rejected redirect leaves this request unchanged. The
  // kept prefix comes from the old target, which is valid, and ends between two percent-escapes.
  if (!IsValidRequestTargetChars(location) ||
      newTargetLen - originFormPos > MaxTargetLen(HttpMessage::kHeaderPosNbBits, _originKeyLen)) {
    return RedirectOutcome::Invalid;
  }

  const int64_t diffLen = static_cast<int64_t>(newTargetLen) - static_cast<int64_t>(oldTargetLen);

  HeadGrowthManager headGrowthManager(*this, diffLen);

  char* pTarget = targetBeg();

  // Move everything after the target (" HTTP/1.1"...).
  std::memmove(pTarget + newTargetLen, pTarget + oldTargetLen,
               _data.size() - oldTargetLen - static_cast<std::size_t>(pTarget - _data.data()));

  // Write 'location' after the kept prefix.
  pTarget += prefixLen;
  if (addSlash) {
    *pTarget++ = '/';
  }
  Copy(location, pTarget);

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
    CompressionState& compressionState = *_opts._pCompressionState;

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
    // finalizeInlineBody() was called. Its output replaces the body, which grows if it does not fit.
    compressionState.makeContext(_opts._pickedEncoding);
    auto& self = const_cast<HttpRequest&>(*this);
    self._data.setSize(bodyStartPos());
    self.appendEncodedInlineOrThrow(decoded);
  }
#endif

  return copy;
}

bool HttpRequest::expectsContinue() const noexcept {
  return bodyLength() != 0 && CaseInsensitiveEqual(headerValueOrEmpty(http::Expect), http::h100_continue);
}

}  // namespace aeronet
