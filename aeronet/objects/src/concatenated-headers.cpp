#include "aeronet/concatenated-headers.hpp"

#include <cassert>
#include <format>
#include <stdexcept>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/reserved-headers.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/tolower-str.hpp"

namespace aeronet {

void Validate(const ConcatenatedHeaders& headers, HeaderType type) {
  for (std::string_view headerNameValue : headers) {
    const auto colonPos = headerNameValue.find(http::HeaderSep);
    if (colonPos == std::string_view::npos) {
      throw std::invalid_argument("header missing http::HeaderSep separator in global headers");
    }

    // Normalize header name to lower-case for case-sensitive lookup later (e.g. for reserved headers, forbidden
    // headers, etc.)
    tolower(const_cast<char*>(headerNameValue.data()), colonPos);

    std::string_view headerName = headerNameValue.substr(0, colonPos);

    if (!http::IsValidHeaderName(headerName)) {
      throw std::invalid_argument(std::format("header has invalid name: '{}'", headerName));
    }

    std::string_view headerValue = headerNameValue.substr(colonPos + http::HeaderSep.size());
    if (TrimOws(headerValue) != headerValue) {
      throw std::invalid_argument(std::format("header value has leading/trailing OWS: '{}'", headerValue));
    }
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
  }
}

}  // namespace aeronet