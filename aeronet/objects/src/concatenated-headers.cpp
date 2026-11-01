#include "aeronet/concatenated-headers.hpp"

#include <cassert>
#include <format>
#include <stdexcept>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/reserved-headers.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/tolower-str.hpp"

namespace aeronet {

namespace {

void ValidateRequestHeader(LowerAsciiKey headerName) {
  if (http::IsReservedOrForbiddenRequestHeader(headerName)) {
    throw std::invalid_argument(
        std::format("attempt to set reserved request header: '{}'", std::string_view(headerName)));
  }
  // Not reserved (a request with a body may carry it), but a client must not send a 100-continue expectation in a
  // request without content (RFC 9110 §10.1.1), as most requests sent with the global headers are.
  if (headerName == http::Expect) {
    throw std::invalid_argument("attempt to set expect as a global header: only a request with a body may carry it");
  }
}

void ValidateResponseHeader(LowerAsciiKey headerName) {
  if (http::IsReservedResponseHeader(headerName)) {
    throw std::invalid_argument(std::format("attempt to set reserved header: '{}'", std::string_view(headerName)));
  }
  // Not reserved (a handler sets it with HttpResponse::location()), but specific to each response: a global one
  // would be on every response, and duplicated (the global first, which clients follow) in the redirects that the
  // server builds itself on a response starting with the global headers.
  if (headerName == http::Location) {
    throw std::invalid_argument("attempt to set location as a global header: its value is specific to each response");
  }
}

}  // namespace

void Validate(const ConcatenatedHeaders& headers, HeaderType type) {
  for (std::string_view headerNameValue : headers) {
    const auto colonPos = headerNameValue.find(http::HeaderSep);
    if (colonPos == std::string_view::npos) {
      throw std::invalid_argument("header missing http::HeaderSep separator in global headers");
    }

    // Normalize header name to lower-case for case-sensitive lookup later (e.g. for reserved headers, forbidden
    // headers, etc.)
    tolower(const_cast<char*>(headerNameValue.data()), colonPos);

    LowerAsciiKey headerName(headerNameValue.substr(0, colonPos));

    if (!http::IsValidHeaderName(headerName)) {
      throw std::invalid_argument(std::format("header has invalid name: '{}'", std::string_view(headerName)));
    }

    std::string_view headerValue = headerNameValue.substr(colonPos + http::HeaderSep.size());
    if (TrimOws(headerValue) != headerValue) {
      throw std::invalid_argument(std::format("header value has leading/trailing OWS: '{}'", headerValue));
    }
    if (!http::IsValidHeaderValue(headerValue)) {
      throw std::invalid_argument(std::format("header has invalid value: '{}'", headerValue));
    }

    if (type == HeaderType::Response) {
      ValidateResponseHeader(headerName);
    } else {
      assert(type == HeaderType::Request);
      ValidateRequestHeader(headerName);
    }
  }
}

}  // namespace aeronet