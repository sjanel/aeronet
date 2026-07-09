#include "aeronet/http-client-config.hpp"

#include <limits>
#include <stdexcept>

#include "aeronet/client-protocol.hpp"

namespace aeronet {

void HttpClientConfig::validate() const {
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