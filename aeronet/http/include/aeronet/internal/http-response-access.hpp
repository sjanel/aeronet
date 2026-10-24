#pragma once

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
};

}  // namespace aeronet::internal