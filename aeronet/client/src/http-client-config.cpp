#include "aeronet/http-client-config.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

#include "aeronet/client-protocol.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/reserved-headers.hpp"
#include "aeronet/string-trim.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/tolower-str.hpp"
#endif

#if !defined(AERONET_ENABLE_ZLIB) || !defined(AERONET_ENABLE_ZSTD) || !defined(AERONET_ENABLE_BROTLI)
#include "aeronet/encoding.hpp"
#endif

namespace aeronet {

HttpClientConfig& HttpClientConfig::withGlobalHeaders(std::span<const http::Header> headers) {
  globalHeaders.clear();
  std::ranges::for_each(headers, [this](const http::Header& header) { globalHeaders.append(header.http1Raw()); });
  return *this;
}

void HttpClientConfig::validate() const {
  decompression.validate();  // no-op when disabled
  if (requestCompression.enabled()) {
    requestCompression.codec.validate();
#if !defined(AERONET_ENABLE_ZLIB) || !defined(AERONET_ENABLE_ZSTD) || !defined(AERONET_ENABLE_BROTLI)
    if (!IsEncodingEnabled(requestCompression.encoding)) {
      throw std::invalid_argument("requestCompression.encoding is not a supported / compiled-in content coding");
    }
#endif
  }

  for (std::string_view headerNameValue : globalHeaders) {
    const auto colonPos = headerNameValue.find(http::HeaderSep);
    if (colonPos == std::string_view::npos) {
      throw std::invalid_argument("header missing http::HeaderSep separator in client global headers");
    }

    std::string_view headerName = headerNameValue.substr(0, colonPos);

    if (http::IsReservedOrForbiddenRequestHeader(headerName)) {
      throw std::invalid_argument(std::format("attempt to set reserved request header: '{}'", headerName));
    }

    if (!http::IsValidHeaderName(headerName)) {
      throw std::invalid_argument(std::format("header has invalid name: '{}'", headerName));
    }

    std::string_view headerValue = TrimOws(headerNameValue.substr(colonPos + 1));
    if (!http::IsValidHeaderValue(headerValue)) {
      throw std::invalid_argument(std::format("header has invalid value: '{}'", headerValue));
    }

#ifdef AERONET_ENABLE_HTTP2
    // forces lower-case header names for HTTP/2
    tolower(const_cast<char*>(headerName.data()), headerName.size());
#endif
  }

  const auto connectMs = connectTimeout.count();
  if (connectMs < 1 || connectMs > std::numeric_limits<int>::max()) {
    throw std::invalid_argument("connectTimeout must be between 1 ms and INT_MAX ms");
  }
  if (httpVersion != HttpVersionMode::Http1_1) {
#ifdef AERONET_ENABLE_HTTP2
    http2.validate();
#else
    if (httpVersion == HttpVersionMode::Http2) {
      throw std::invalid_argument("httpVersion requires HTTP/2 but aeronet was built without AERONET_ENABLE_HTTP2");
    }
#endif
    if (http2.enablePush) {
      throw std::invalid_argument("HTTP/2 client cannot enable server push");
    }
  }
  if (cache.enabled()) {
    if (cache.maxEntries == 0) {
      throw std::invalid_argument("cache.maxEntries must be at least 1 when the response cache is enabled");
    }
    // Only safe methods may be cached: caching an unsafe / non-idempotent method's response is nonsensical.
    static constexpr http::MethodBmp kCacheableMethods = http::Method::GET | http::Method::HEAD | http::Method::OPTIONS;
    if ((cache.methods & ~kCacheableMethods) != 0 || cache.methods == 0) {
      throw std::invalid_argument("cache.methods must be a non-empty subset of GET / HEAD / OPTIONS");
    }
  }
}

}  // namespace aeronet