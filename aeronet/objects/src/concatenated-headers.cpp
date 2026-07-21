#include "aeronet/concatenated-headers.hpp"

#include <cassert>
#include <format>
#include <stdexcept>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/reserved-headers.hpp"
#include "aeronet/string-trim.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/tolower-str.hpp"
#endif

namespace aeronet {

void Validate(const ConcatenatedHeaders& headers, HeaderType type) {
  for (std::string_view headerNameValue : headers) {
    const auto colonPos = headerNameValue.find(http::HeaderSep);
    if (colonPos == std::string_view::npos) {
      throw std::invalid_argument("header missing http::HeaderSep separator in global headers");
    }

    std::string_view headerName = headerNameValue.substr(0, colonPos);

    if (!http::IsValidHeaderName(headerName)) {
      throw std::invalid_argument(std::format("header has invalid name: '{}'", headerName));
    }

    std::string_view headerValue = TrimOws(headerNameValue.substr(colonPos + 1));
    if (!http::IsValidHeaderValue(headerValue)) {
      throw std::invalid_argument(std::format("header has invalid value: '{}'", headerValue));
    }

    if (type == HeaderType::Response) {
      if (http::IsReservedResponseHeader(headerName)) {
        throw std::invalid_argument(std::format("attempt to set reserved header: '{}'", headerName));
      }
    } else {
      assert(type == HeaderType::Request);
      if (http::IsReservedOrForbiddenRequestHeader(headerName)) {
        throw std::invalid_argument(std::format("attempt to set reserved request header: '{}'", headerName));
      }
    }

#ifdef AERONET_ENABLE_HTTP2
    // forces lower-case header names for HTTP/2
    tolower(const_cast<char*>(headerName.data()), headerName.size());
#endif
  }
}

}  // namespace aeronet