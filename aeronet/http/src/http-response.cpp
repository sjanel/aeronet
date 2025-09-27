#include "aeronet/http-response.hpp"

#include <cassert>
#include <cstddef>
#include <string_view>

#include "http-constants.hpp"
#include "http-response-build.hpp"
#include "raw-chars.hpp"
#include "stringconv.hpp"

namespace aeronet::http {

RawChars buildHead(HttpResponse &resp, std::string_view httpVersion, std::string_view date, bool keepAlive) {
  std::string_view keepAliveStr = keepAlive ? http::keepalive : http::close;

  auto bodySize = resp.body().size();

  // Base headers: Date, Content-Type, Content-Length, Connection (+ optional Location)
  // Compute size conservatively: base + headers
  const std::size_t size = httpVersion.size() + 2U + 3U + resp.reason().size() +
                           // CRLF after status line
                           http::CRLF.size() +
                           // Custom headers
                           resp.headersTotalLen() +
                           // Date header
                           http::Date.size() + http::HeaderSep.size() + date.size() + http::CRLF.size() +
                           // Content-Length
                           http::ContentLength.size() + http::HeaderSep.size() +
                           static_cast<std::size_t>(nchars(bodySize)) + http::CRLF.size() +
                           // Connection
                           http::Connection.size() + http::HeaderSep.size() + keepAliveStr.size() +
                           http::DoubleCRLF.size();

  RawChars header(size);

  // Trust caller to validate version (only HTTP/1.0 or HTTP/1.1 allowed upstream)
  header.unchecked_append(httpVersion);
  header.unchecked_push_back(' ');
  header.unchecked_append(std::string_view(IntegralToCharVector(resp.statusCode())));
  header.unchecked_push_back(' ');
  header.unchecked_append(resp.reason());
  header.unchecked_append(http::CRLF);
  // Custom headers
  for (const auto &[name, value] : resp.headers()) {
    header.unchecked_append(name);
    header.unchecked_append(http::HeaderSep);
    header.unchecked_append(value);
    header.unchecked_append(http::CRLF);
  }
  // Date
  header.unchecked_append(http::Date);
  header.unchecked_append(http::HeaderSep);
  header.unchecked_append(date);
  header.unchecked_append(http::CRLF);
  // Content-Length
  header.unchecked_append(http::ContentLength);
  header.unchecked_append(http::HeaderSep);
  header.unchecked_append(std::string_view(IntegralToCharVector(bodySize)));
  header.unchecked_append(http::CRLF);
  // Connection
  header.unchecked_append(http::Connection);
  header.unchecked_append(http::HeaderSep);
  header.unchecked_append(keepAliveStr);
  header.unchecked_append(http::DoubleCRLF);
  assert(header.size() == size);
  return header;
}

}  // namespace aeronet::http
