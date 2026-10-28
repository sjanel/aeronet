#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/compiler-config.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/lower-ascii-key.hpp"

namespace aeronet::internal {

class HttpResponseAccess {
 public:
  AERONET_ALWAYS_INLINE static HttpResponse MakeUnallocated() noexcept { return HttpResponse(HttpResponse::Check::No); }

  AERONET_ALWAYS_INLINE static void HeaderAddLineUnchecked(HttpResponse& response, LowerAsciiKey key,
                                                           std::string_view value) {
    response.headerAddLineUnchecked(key, value);
  }

  // See HttpResponse::LocationHeaderSize
  [[nodiscard]] AERONET_ALWAYS_INLINE static std::size_t LocationHeaderSize(std::string_view location, bool addSlash) {
    return HttpResponse::LocationHeaderSize(location, addSlash);
  }

  // See HttpResponse::headerAddLocationUnchecked
  AERONET_ALWAYS_INLINE static void HeaderAddLocationUnchecked(HttpResponse& response, std::string_view location,
                                                               bool addSlash) {
    response.headerAddLocationUnchecked(location, addSlash);
  }
};

}  // namespace aeronet::internal