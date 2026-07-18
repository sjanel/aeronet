#include "aeronet/http-request.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "aeronet/char-hexadecimal-converter.hpp"
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
#include "aeronet/memory-utils.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "url-parse.hpp"

namespace aeronet {

namespace {

constexpr std::string_view kSchemeSep = "://";

constexpr bool IsHex(unsigned char ch) noexcept {
  return (ch >= '0' && ch <= '9') || ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'f');
}

bool IsValidRequestTarget(std::string_view target) noexcept {
  if (target.empty()) {
    return false;
  }

  const auto* pData = reinterpret_cast<const unsigned char*>(target.data());
  const auto* end = pData + target.size();

  while (pData != end) {
    unsigned char ch = *pData++;

    // RFC 9112 :
    // request-target must not contain SP, CTL or DEL.

    if (ch <= 0x20 || ch == 0x7F) {
      return false;
    }

    if (ch == '%') {
      if (end - pData < 2) {
        return false;
      }

      if (!IsHex(pData[0]) || !IsHex(pData[1])) {
        return false;
      }

      pData += 2;
    }
  }

  return true;
}

constexpr const char* CheckTarget(std::string_view target, uint8_t headerPosNbBits, uint32_t originKeyLen) {
  if (!IsValidRequestTarget(target)) {
    return "Invalid HTTP request target";
  }

  static constexpr std::uint32_t kMaxMethodStrLen = static_cast<uint32_t>(
      std::ranges::max_element(http::kMethodStrings, {}, [](std::string_view str) { return str.size(); })->size());

  const uint32_t maxTargetLen = static_cast<uint32_t>((1U << headerPosNbBits) - 1U - kMaxMethodStrLen - 2U -
                                                      http::HTTP10Sv.size() - originKeyLen);
  if (target.size() > maxTargetLen) {
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
  std::size_t sz = http::MethodToStr(method).size() + 1U;
  if (hasNonTlsProxy) {
    // Absolute-form request-target for a cleartext proxy: "scheme://host:port" (the origin key) + origin-form.
    sz += originKeyLen + target.size();
  } else {
    sz += target.size();
  }
  return sz + 1U + http::HTTP10Sv.size() + http::DoubleCRLF.size();
}

inline char* AppendScheme(bool isTls, char* pData) {
  if (isTls) {
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
    std::memcpy(pData, "https://", 8U);
    pData += 8U;
  } else {
    // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
    std::memcpy(pData, "http://", 7U);
    pData += 7U;
  }
  return pData;
}

inline char* InitData(http::Method method, bool hasNonTlsProxy, bool hostIsIpv6,
                      const internal::UrlParseResult& urlParseResult, char* pData) {
  // Write origin key at beginning of buffer: "scheme://host:port" (RFC 9112 section 3.2.1, 3.2.2, 9.3.6).
  char* const pOriginKeyBeg = pData;
  pData = AppendScheme(urlParseResult.isTls, pData);
  pData = Append(urlParseResult.host, pData);
  *pData++ = ':';  // port is always specified in origin key
  pData = std::to_chars(pData, pData + std::numeric_limits<uint16_t>::max(), urlParseResult.port).ptr;
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
  pData = Append(http::HTTP11Sv, pData);

  // Host Header
  pData = Append(http::CRLF, pData);
  pData = Append(http::Host, pData);
  pData = Append(http::HeaderSep, pData);
  if (hostIsIpv6) {
    *pData++ = '[';
  }
  pData = Append(urlParseResult.host, pData);
  if (hostIsIpv6) {
    *pData++ = ']';
  }
  if (urlParseResult.hasNonDefaultPort()) {
    *pData++ = ':';
    pData = std::to_chars(pData, pData + std::numeric_limits<uint16_t>::max(), urlParseResult.port).ptr;
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

HttpRequest::HttpRequest(http::Method method, std::string_view url, std::string_view concatenatedHeaders, Options opts)
    : HttpMessage(opts) {
  const auto res = internal::ParseUrl(url);
  if (res.host.empty()) {
    throw std::invalid_argument("Invalid URL");
  }

  const auto portNbChars = nchars(res.port);
  const bool hasNonTlsProxy = opts.hasProxy() && !res.isTls;
  const auto schemeLen = static_cast<uint8_t>((res.isTls ? internal::kHttps : internal::kHttp).size());

  _hostLen = SafeCast<decltype(_hostLen)>(res.host.size());
  _port = res.port;
  _originKeyLen = SafeCast<decltype(_originKeyLen)>(schemeLen + kSchemeSep.size() + _hostLen + 1U + portNbChars);

  const bool hostIsIpv6 = res.host.contains(':');
  auto hostHeaderSize = ComputeHostHeaderSize(res.host, hostIsIpv6, res.hasNonDefaultPort(), portNbChars);

  const auto neededCapacity =
      HttpRequestInitialSize(method, hasNonTlsProxy, HttpMessage::kHeaderPosNbBits, _originKeyLen, res.target) +
      hostHeaderSize + concatenatedHeaders.size() + _originKeyLen;

  setBodyStartPos(neededCapacity);

  _data.reserve(neededCapacity);

  char* pInsert = InitData(method, hasNonTlsProxy, hostIsIpv6, res, _data.data());
  setHeadersStartPos(static_cast<uint64_t>(pInsert - _data.data()) - hostHeaderSize);
  if (!concatenatedHeaders.empty()) {
    pInsert = Append(http::CRLF, pInsert);
    pInsert = Append(concatenatedHeaders, pInsert);
    pInsert -= http::CRLF.size();  // remove the last CRLF
  }
  pInsert = Append(http::DoubleCRLF, pInsert);
  _data.setSize(static_cast<uint64_t>(pInsert - _data.data()));

  assert(_data.size() == _data.capacity());
}

HttpRequest::HttpRequest(std::size_t additionalCapacity, http::Method method, std::string_view url,
                         std::string_view concatenatedHeaders, Options opts, std::string_view body,
                         std::string_view contentType)
    : HttpMessage(opts) {
  const auto res = internal::ParseUrl(url);

  if (res.host.empty()) {
    throw std::invalid_argument("Invalid URL");
  }

  const auto portNbChars = nchars(res.port);
  const bool hasNonTlsProxy = opts.hasProxy() && !res.isTls;
  const auto schemeLen = static_cast<uint8_t>((res.isTls ? internal::kHttps : internal::kHttp).size());

  _hostLen = SafeCast<decltype(_hostLen)>(res.host.size());
  _port = res.port;
  _originKeyLen = SafeCast<decltype(_originKeyLen)>(schemeLen + kSchemeSep.size() + _hostLen + 1U + portNbChars);

  const bool hostIsIpv6 = res.host.contains(':');
  const auto hostHeaderSize = ComputeHostHeaderSize(res.host, hostIsIpv6, res.hasNonDefaultPort(), portNbChars);

  const auto neededCapacity =
      HttpRequestInitialSize(method, hasNonTlsProxy, HttpMessage::kHeaderPosNbBits, _originKeyLen, res.target) +
      hostHeaderSize + concatenatedHeaders.size() +
      NeededBodyHeadersSize(body.size(), CheckContentType(body.empty(), contentType).size()) + body.size() +
      additionalCapacity + _originKeyLen;

  _data.reserve(neededCapacity);

  char* pInsert = InitData(method, hasNonTlsProxy, hostIsIpv6, res, _data.data());
  setHeadersStartPos(static_cast<uint64_t>(pInsert - _data.data()) - hostHeaderSize);
  if (!concatenatedHeaders.empty()) {
    pInsert = Append(http::CRLF, pInsert);
    pInsert = Append(concatenatedHeaders, pInsert);
    pInsert -= http::CRLF.size();  // remove the last CRLF
  }
  if (!body.empty()) {
    pInsert = WriteCRLFHeader(http::ContentType, contentType, pInsert);
    pInsert = WriteCRLFHeader(http::ContentLength, body.size(), pInsert);
    pInsert = Append(http::DoubleCRLF, pInsert);
    pInsert = Append(body, pInsert);
  } else {
    pInsert = Append(http::DoubleCRLF, pInsert);
  }
  _data.setSize(static_cast<uint64_t>(pInsert - _data.data()));
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
  const int8_t diffLen = static_cast<int8_t>(newMethodLen) - static_cast<int8_t>(oldMethodLen);

  _data.ensureAvailableCapacityExponential(diffLen);

  // Shift the [method-end, end) tail by `diffLen` to make room for the new method string.
  assert(_data.size() >= oldMethodLen);
  std::memmove(_data.data() + newMethodLen + _originKeyLen, _data.data() + oldMethodLen + _originKeyLen,
               _data.size() - oldMethodLen - _originKeyLen);

  Copy(newMethodStr, _data.data() + _originKeyLen);

  // Adjust positions and size
  adjustHeadersAndBodyStart(diffLen);

  _data.adjustSize(diffLen);

  return *this;
}

HttpRequest& HttpRequest::target(std::string_view target) & {
  CheckTargetOrThrow(target, HttpMessage::kHeaderPosNbBits, _originKeyLen);

  const auto oldTarget = this->target();

  const auto oldTargetLen = oldTarget.size();
  const auto newTargetLen = target.size();

  const int32_t diffLen = static_cast<int32_t>(newTargetLen) - static_cast<int32_t>(oldTargetLen);

  _data.ensureAvailableCapacityExponential(diffLen);

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
  const auto* pHostHeaderEnd =
      SearchCRLF(_data.data() + http::CRLF.size() + headersStartPos() + http::Host.size() + http::HeaderSep.size(),
                 _data.data() + _data.size());
  const auto oldHostHeaderEndPos = static_cast<uint64_t>(pHostHeaderEnd - _data.data());

  const auto portNbChars = nchars(res.port);
  const bool hasNonTlsProxy = _opts.hasProxy() && !res.isTls;
  const auto method = this->method();
  const auto methodLen = this->methodLen();
  const auto schemeLen = static_cast<uint8_t>((res.isTls ? internal::kHttps : internal::kHttp).size());

  _hostLen = SafeCast<decltype(_hostLen)>(res.host.size());
  _port = res.port;
  _originKeyLen = SafeCast<decltype(_originKeyLen)>(schemeLen + kSchemeSep.size() + _hostLen + 1U + portNbChars);

  const char* pErrorMsg = CheckTarget(res.target, HttpMessage::kHeaderPosNbBits, _originKeyLen);
  if (pErrorMsg != nullptr) {
    return pErrorMsg;
  }

  const bool hostIsIpv6 = res.host.contains(':');
  const auto hostHeaderSize = ComputeHostHeaderSize(res.host, hostIsIpv6, res.hasNonDefaultPort(), portNbChars);

  const auto newHostHeaderEndPos =
      _originKeyLen + methodLen + 1U + res.target.size() + 1U + http::HTTP11Sv.size() + hostHeaderSize;
  const int32_t diffLen = static_cast<int32_t>(newHostHeaderEndPos) - static_cast<int32_t>(oldHostHeaderEndPos);

  _data.ensureAvailableCapacityExponential(diffLen);

  char* pData = _data.data();

  // Move everything after the origin key.
  std::memmove(pData + newHostHeaderEndPos, pData + oldHostHeaderEndPos, _data.size() - oldHostHeaderEndPos);

  char* pInsert = InitData(method, hasNonTlsProxy, hostIsIpv6, res, _data.data());

  setHeadersStartPos(static_cast<uint64_t>(pInsert - _data.data()) - hostHeaderSize);
  adjustBodyStart(diffLen);
  _data.adjustSize(diffLen);
  return pErrorMsg;
}

bool HttpRequest::resolveRedirect(std::string_view location) {
  if (location.contains("://")) {
    // absolute URL, parse it and set the new origin key
    const auto res = internal::ParseUrl(location);
    return !res.host.empty() && setNewUrl(res) == nullptr;
  }

  // Network-path reference: //host[:port][/path] -> inherit scheme. Parse the authority directly and
  // build the canonical buffer once, instead of synthesizing a "scheme://..." string to re-parse and copy.
  if (location.starts_with("//")) {
    internal::UrlParseResult res;
    res.isTls = isTlsRequest();
    internal::ParseAuthority(location.substr(2), res);
    return !res.host.empty() && setNewUrl(res) == nullptr;
  }

  // Strip fragment from the relative reference.
  if (const auto hashPos = location.find('#'); hashPos != std::string_view::npos) {
    location = location.substr(0, hashPos);
  }

  if (location.starts_with('/')) {
    // Absolute path: keep origin, replace target.
    target(location);
    return true;
  }

  std::string_view base = target();
  const auto oldTargetLen = base.size();
  if (const auto questionMarkPos = base.find('?'); questionMarkPos != std::string_view::npos) {
    base = base.substr(0, questionMarkPos);
  }

  uint32_t prefixLen;

  if (location.starts_with('?')) {
    // Keep the whole path and append the new query.
    prefixLen = static_cast<uint32_t>(oldTargetLen);
  } else {
    // Keep only the directory (including the trailing '/').
    const auto lastSlashPos = base.rfind('/');
    prefixLen = lastSlashPos == std::string_view::npos ? 1U : static_cast<uint32_t>(lastSlashPos + 1);
  }

  const auto newTargetLen = prefixLen + location.size();
  const int32_t diffLen = static_cast<int32_t>(newTargetLen) - static_cast<int32_t>(oldTargetLen);

  _data.ensureAvailableCapacityExponential(diffLen);

  char* pTarget = targetBeg();

  // Move everything after the target (" HTTP/1.1"...).
  std::memmove(pTarget + newTargetLen, pTarget + oldTargetLen,
               _data.size() - oldTargetLen - static_cast<std::size_t>(pTarget - _data.data()));

  // Overwrite the suffix.
  char* pEndTarget = Append(location, pTarget + prefixLen);

  const char* pErrorMsg =
      CheckTarget(std::string_view(pTarget, pEndTarget), HttpMessage::kHeaderPosNbBits, _originKeyLen);
  if (pErrorMsg != nullptr) {
    return false;
  }

  adjustHeadersAndBodyStart(diffLen);
  _data.adjustSize(diffLen);

  return true;
}

// Finalizes the HttpRequest and returns an HttpMessageData object that can be sent over the network.
// After calling this function, the HttpRequest object is still valid and can be reused to build another request,
// but the HttpMessageData has been created by copy. To avoid the copy, use the rvalue overload below.
[[nodiscard]] HttpRequest HttpRequest::finalize(internal::HttpClientCodec& clientCodec,
                                                const DecompressionConfig& decompressionConfig) const {
  HttpRequest copy(HttpMessage::Check::No);

  copy._data = _data;
  copy._posBitmap = _posBitmap;
  copy._payloadVariant = _payloadVariant;
  copy._opts = _opts;

  copy._hostLen = _hostLen;
  copy._port = _port;
  copy._originKeyLen = _originKeyLen;

  copy.prepareForFinalization();

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  if (_opts.isAutomaticDirectCompression() && copy.trailersSize() == 0) {
    // We need to restore current HttpRequest compression state to make it as if finalize() was never called, so that
    // the current HttpRequest can be reused. This is because the compression state is shared between the current
    // HttpRequest and the copy, and finalizeInlineBody() modifies the compression state.
    auto& compressionState = *_opts._pCompressionState;

    // Then, we need to decompress the compressed data in the copy and restore the encoder state to what it was before
    // finalizeInlineBody() was called
    // TODO: check that it's ok to reuse the clientCodec.decompressOut here.
    std::string_view decoded;
    const auto decodeRes = internal::HttpCodec::DecompressFullBody(
        clientCodec.decompressionState, decompressionConfig, headerValueOrEmpty(http::ContentEncoding),
        copy.bodyInMemory(), clientCodec.decompressOut, clientCodec.decompressTmp, decoded);
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
    const_cast<HttpRequest&>(*this)._data.setSize(bodyStartPos + result.written());
  }
#endif

  return copy;
}

void HttpRequest::writeChunkedRequestForHttp11(RawChars& out) const {
  assert(trailersSize() != 0);
  assert(!hasBodyFile());

  const std::string_view completeRequest = completeRequestForHttp11();
  const std::string_view requestLine = completeRequest.substr(0, statusLineSize());
  const std::string_view body = bodyInMemory();
  const std::string_view trailersView = trailersFlatView();
  const std::size_t bodyLen = body.size();
  const bool addTrailerHeader = _opts.isAddTrailerHeader();

  // Generous single reservation: the original request bytes plus the chunk framing overhead (hex length,
  // CRLFs, "0" terminator), the Transfer-Encoding header and (optionally) the Trailer header. No further
  // (re)allocation happens after this, so the raw append pointer below stays valid.
  out.reserve(completeRequest.size() + trailersView.size() + hex_digits(bodyLen) + kTransferEncodingChunkedCRLF.size() +
              http::HeaderSize(http::Trailer.size(), 0) + 64U);

  char* pEnd = out.data();
  pEnd = Append(requestLine, pEnd);

  // Keep every header except Content-Length (the chunked body has no fixed length). Content-Type, Host,
  // User-Agent, Accept-Encoding and any user header are preserved verbatim.
  for (const auto& [name, value] : headers()) {
    if (CaseInsensitiveEqual(name, http::ContentLength)) {
      continue;
    }
    pEnd = Append(name, pEnd);
    pEnd = Append(http::HeaderSep, pEnd);
    pEnd = Append(value, pEnd);
    pEnd = Append(http::CRLF, pEnd);
  }

  pEnd = Append(http::TransferEncoding, pEnd);
  pEnd = Append(http::HeaderSep, pEnd);
  pEnd = Append(http::chunked, pEnd);
  pEnd = Append(http::CRLF, pEnd);

  // Optional Trailer header advertising the trailer field names (RFC 7230 section 4.4).
  if (addTrailerHeader) {
    pEnd = Append(http::Trailer, pEnd);
    pEnd = Append(http::HeaderSep, pEnd);
    bool first = true;
    for (const auto& [name, value] : trailers()) {
      if (!first) {
        pEnd = Append(kTrailerValueSep, pEnd);
      }
      pEnd = Append(name, pEnd);
      first = false;
    }
    pEnd = Append(http::CRLF, pEnd);
  }

  // End of headers.
  pEnd = Append(http::CRLF, pEnd);

  // Single chunk carrying the whole body, then the zero-length terminating chunk followed by the trailers.
  pEnd = to_lower_hex(bodyLen, pEnd);
  pEnd = Append(http::CRLF, pEnd);
  pEnd = Append(body, pEnd);
  pEnd = Append(http::CRLF, pEnd);
  *pEnd++ = '0';
  pEnd = Append(http::CRLF, pEnd);
  pEnd = Append(trailersView, pEnd);  // each trailer line already ends with CRLF
  pEnd = Append(http::CRLF, pEnd);    // blank line terminating the trailer section

  out.setSize(static_cast<std::size_t>(pEnd - out.data()));
}

}  // namespace aeronet