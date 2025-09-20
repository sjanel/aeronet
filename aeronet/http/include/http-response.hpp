#pragma once

#include <string_view>

#include "string.hpp"

namespace aeronet {

struct HttpResponse {
  int statusCode{200};
  string reason{"OK"};
  string body{"Hello from aeronet"};
  string contentType{"text/plain"};

  // Build an HTTP/1.1 response head (status line + basic headers + final CRLFCRLF).
  // bodySize allows specifying the length that would be sent (e.g. for HEAD requests
  // where the body itself is not transmitted). The date must be a preformatted RFC 7231
  // timestamp string. keepAlive controls the Connection header value.
  [[nodiscard]] string buildHead(std::string_view httpVersion, std::string_view date, bool keepAlive,
                                 std::size_t bodySize) const;
};

}  // namespace aeronet