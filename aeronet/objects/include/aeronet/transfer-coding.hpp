#pragma once

#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet::http {

// Whether the final coding of a (possibly comma-separated) Transfer-Encoding value is chunked, which then frames the
// message body (RFC 9112 §6.3).
constexpr bool LastTransferCodingIsChunked(std::string_view transferEncoding) {
  const auto comma = transferEncoding.rfind(',');
  const std::string_view last = comma == std::string_view::npos ? transferEncoding : transferEncoding.substr(comma + 1);
  return CaseInsensitiveEqual(TrimOws(last), chunked);
}

}  // namespace aeronet::http
