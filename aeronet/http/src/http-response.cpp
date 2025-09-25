#include "aeronet/http-response.hpp"

#include <cstddef>
#include <string_view>

#include "http-constants.hpp"
#include "http-response-build.hpp"
#include "nchars.hpp"
#include "raw-chars.hpp"
#include "stringconv.hpp"

namespace aeronet::http {

RawChars buildHead(const HttpResponse &resp, std::string_view httpVersion, std::string_view date, bool keepAlive,
                   std::size_t bodySize) {
  static constexpr std::string_view kSep = ": ";
  static constexpr std::string_view kEnd = "\r\n\r\n";

  std::string_view keepAliveStr = keepAlive ? http::keepalive : http::close;

  RawChars header(httpVersion.size() + 2U + static_cast<std::size_t>(nchars(resp.statusCode)) + resp.reason.size() +
                  (4U * http::CRLF.size()) + http::Date.size() + (4U * kSep.size()) + date.size() +
                  http::ContentType.size() + resp.contentType.size() + http::ContentLength.size() +
                  static_cast<std::size_t>(nchars(bodySize)) + http::Connection.size() + keepAliveStr.size() +
                  kEnd.size());

  // Trust caller to validate version (only HTTP/1.0 or HTTP/1.1 allowed upstream)
  header.unchecked_append(httpVersion);
  header.unchecked_push_back(' ');
  header.unchecked_append(std::string_view(IntegralToCharVector(resp.statusCode)));
  header.unchecked_push_back(' ');
  header.unchecked_append(resp.reason);
  header.unchecked_append(http::CRLF);
  header.unchecked_append(http::Date);
  header.unchecked_append(kSep);
  header.unchecked_append(date);
  header.unchecked_append(http::CRLF);
  header.unchecked_append(http::ContentType);
  header.unchecked_append(kSep);
  header.unchecked_append(resp.contentType);
  header.unchecked_append(http::CRLF);
  header.unchecked_append(http::ContentLength);
  header.unchecked_append(kSep);
  header.unchecked_append(std::string_view(IntegralToCharVector(bodySize)));
  header.unchecked_append(http::CRLF);
  header.unchecked_append(http::Connection);
  header.unchecked_append(kSep);
  header.unchecked_append(keepAliveStr);
  header.unchecked_append(kEnd);
  return header;
}

}  // namespace aeronet::http
