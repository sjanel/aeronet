#include "http-response.hpp"

#include <cstddef>
#include <string_view>

#include "http-constants.hpp"
#include "http-response-build.hpp"
#include "raw-chars.hpp"
#include "stringconv.hpp"

namespace aeronet::http {

RawChars buildHead(const HttpResponse &resp, std::string_view httpVersion, std::string_view date, bool keepAlive,
                   std::size_t bodySize) {
  static constexpr std::string_view kSep = ": ";
  static constexpr std::string_view kEnd = "\r\n\r\n";

  std::string_view keepAliveStr = keepAlive ? http::keepalive : http::close;

  RawChars header(httpVersion.size() + 2U + static_cast<std::size_t>(nchars(resp.statusCode)) + resp.reason.size() +
                  http::CRLF.size() + http::Date.size() + kSep.size() + date.size() + http::CRLF.size() +
                  http::ContentType.size() + kSep.size() + resp.contentType.size() + http::CRLF.size() +
                  http::ContentLength.size() + kSep.size() + static_cast<std::size_t>(nchars(bodySize)) +
                  http::CRLF.size() + http::Connection.size() + kSep.size() + keepAliveStr.size() + kEnd.size());

  // Trust caller to validate version (only HTTP/1.0 or HTTP/1.1 allowed upstream)
  header.append(httpVersion);
  header.append(' ');
  header.append(std::string_view(IntegralToCharVector(resp.statusCode)));
  header.append(' ');
  header.append(resp.reason);
  header.append(http::CRLF);
  header.append(http::Date);
  header.append(kSep);
  header.append(date);
  header.append(http::CRLF);
  header.append(http::ContentType);
  header.append(kSep);
  header.append(resp.contentType);
  header.append(http::CRLF);
  header.append(http::ContentLength);
  header.append(kSep);
  header.append(std::string_view(IntegralToCharVector(bodySize)));
  header.append(http::CRLF);
  header.append(http::Connection);
  header.append(kSep);
  header.append(keepAliveStr);
  header.append(kEnd);
  return header;
}

}  // namespace aeronet::http
