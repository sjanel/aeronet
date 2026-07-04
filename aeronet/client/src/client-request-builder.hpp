#pragma once

#include <charconv>
#include <cstddef>
#include <string_view>

#include "aeronet/http-client-config.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/url.hpp"
#include "client-accept-encoding.hpp"

namespace aeronet::internal {

// Building blocks shared by the HTTP/1.1 (buildRequestBytesForHttp11) and HTTP/2 (buildHeaderBlockForHttp2)
// request-head builders. They emit different wire formats, but agree on which user headers get special
// treatment, how the outbound body is (optionally) compressed, how the default Accept-Encoding is chosen,
// and how the authority is spelled -- so those policies live here, once, instead of drifting between the two.

// Outcome of scanning an outbound request's user headers once: the values both builders reference and which
// framing headers the user already supplied (so the builder does not inject a duplicate).
struct RequestHeaderScan {
  std::string_view host;         // user-supplied Host value (empty => none); HTTP/2 carries it as :authority
  std::string_view contentType;  // user-supplied Content-Type value (HTTP/1.1 GET-rewrite size accounting)
  bool hasUserAgent{false};
  bool hasAcceptEncoding{false};
  bool hasConnection{false};
  bool hasTransferEncoding{false};
  bool hasContentEncoding{false};
  bool hasContentLength{false};
};

// Scan `req`'s user headers once into a RequestHeaderScan.
[[nodiscard]] inline RequestHeaderScan ScanRequestHeaders(HeadersView headersView) {
  RequestHeaderScan scan;
  for (const auto& [name, value] : headersView) {
    if (CaseInsensitiveEqual(name, http::Host)) {
      scan.host = value;
    } else if (CaseInsensitiveEqual(name, http::UserAgent)) {
      scan.hasUserAgent = true;
    } else if (CaseInsensitiveEqual(name, http::AcceptEncoding)) {
      scan.hasAcceptEncoding = true;
    } else if (CaseInsensitiveEqual(name, http::Connection)) {
      scan.hasConnection = true;
    } else if (CaseInsensitiveEqual(name, http::TransferEncoding)) {
      scan.hasTransferEncoding = true;
    } else if (CaseInsensitiveEqual(name, http::ContentEncoding)) {
      scan.hasContentEncoding = true;
    } else if (CaseInsensitiveEqual(name, http::ContentLength)) {
      scan.hasContentLength = true;
    } else if (CaseInsensitiveEqual(name, http::ContentType)) {
      scan.contentType = value;
    }
  }
  return scan;
}

// Resolve the Accept-Encoding value the builder should inject, or an empty view to inject nothing: an
// explicit config.defaultAcceptEncoding() wins; otherwise, when response decompression is enabled, advertise
// exactly what this build can decode (so origins know they may compress and we transparently decode).
// Returns empty when the user already set Accept-Encoding (`hasAcceptEncoding`).
[[nodiscard]] inline std::string_view ResolveAcceptEncoding(const HttpClientConfig& config, bool hasAcceptEncoding) {
  if (hasAcceptEncoding) {
    return {};
  }
  std::string_view acceptEncoding = config.defaultAcceptEncoding();
  if (acceptEncoding.empty() && config.decompression.enable) {
    acceptEncoding = kSupportedAcceptEncoding;
  }
  return acceptEncoding;
}

// Byte length of the request authority derived from `url`: the (unbracketed) host, plus 2 for the '[' ']'
// around an IPv6 literal, plus ":<port>" when the port is not the scheme default.
[[nodiscard]] inline std::size_t AuthorityLen(const Url& url, bool hostIsIpv6) {
  std::size_t len = url.host().size() + (hostIsIpv6 ? 2U : 0U);
  if (!url.isDefaultPort()) {
    len += 1U + ndigits(url.port());
  }
  return len;
}

// Append the request authority derived from `url` ("[host]:port", brackets only for an IPv6 literal, port
// omitted when default) at `pEnd`, returning the new end pointer. AuthorityLen() bytes must already be
// reserved. Shared by the HTTP/1.1 Host header value and the HTTP/2 :authority pseudo-header.
[[nodiscard]] inline char* AppendAuthority(char* pBuf, const Url& url, bool hostIsIpv6) {
  if (hostIsIpv6) {
    *pBuf++ = '[';
  }
  pBuf = Append(url.host(), pBuf);
  if (hostIsIpv6) {
    *pBuf++ = ']';
  }
  if (!url.isDefaultPort()) {
    *pBuf++ = ':';
    pBuf = std::to_chars(pBuf, pBuf + ndigits(url.port()), url.port()).ptr;
  }
  return pBuf;
}

}  // namespace aeronet::internal
