#include "aeronet/http-client-config.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <span>
#include <stdexcept>

#include "aeronet/client-protocol.hpp"
#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"

#if !defined(AERONET_ENABLE_ZLIB) || !defined(AERONET_ENABLE_ZSTD) || !defined(AERONET_ENABLE_BROTLI)
#include "aeronet/encoding.hpp"
#endif

namespace aeronet {

HttpClientConfig& HttpClientConfig::withGlobalHeaders(std::span<const http::Header> headers) {
  globalHeaders.clear();
  std::ranges::for_each(headers, [this](const http::Header& header) {
    globalHeaders.appendAsHttp1Header(header.name(), header.value());
  });
  return *this;
}

void HttpClientConfig::validate() const {
  const auto connectMs = connectTimeout.count();
  if (connectMs < 1 || connectMs > std::numeric_limits<int>::max()) {
    throw std::invalid_argument("connectTimeout must be between 1 ms and INT_MAX ms");
  }
  if (requestTimeout <= std::chrono::milliseconds{0}) {
    throw std::invalid_argument("requestTimeout must be greater than 0");
  }
  if (keepAliveTimeout < std::chrono::milliseconds{0}) {
    throw std::invalid_argument("keepAliveTimeout must be greater than or equal to 0");
  }
  if (maxResponseBytes < 1U) {
    throw std::invalid_argument("maxResponseBytes must be > 0");
  }
  if (minReadChunkBytes < 1U) {
    throw std::invalid_argument("minReadChunkBytes must be > 0");
  }

  Validate(globalHeaders, HeaderType::Request);

  retry.validate();

  if (httpVersion != HttpVersionMode::Http1_1) {
#ifdef AERONET_ENABLE_HTTP2
    http2.validate();
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
    if ((cache.methods & static_cast<http::MethodBmp>(~kCacheableMethods)) != 0 || cache.methods == 0) {
      throw std::invalid_argument("cache.methods must be a non-empty subset of GET / HEAD / OPTIONS");
    }
  }

  decompression.validate();
  if (requestCompression.enabled()) {
    requestCompression.codec.validate();
#if !defined(AERONET_ENABLE_ZLIB) || !defined(AERONET_ENABLE_ZSTD) || !defined(AERONET_ENABLE_BROTLI)
    if (!IsEncodingEnabled(requestCompression.encoding)) {
      throw std::invalid_argument("requestCompression.encoding is not a supported / compiled-in content coding");
    }
#endif
  }
}

}  // namespace aeronet