#include "http-response.hpp"

#include <cstddef>
#include <string_view>

#include "http-constants.hpp"
#include "string.hpp"
#include "stringconv.hpp"

namespace aeronet {

string HttpResponse::buildHead(std::string_view httpVersion, std::string_view date, bool keepAlive,
                               std::size_t bodySize) const {
  string header;

  // Reuse centralized header field name constants while keeping CRLF + colon spacing local.
  static constexpr std::string_view kCRLF = "\r\n";
  static constexpr std::string_view kSep = ": ";
  static constexpr std::string_view kEnd = "\r\n\r\n";

  std::string_view keepAliveStr = keepAlive ? http::keepalive : http::close;

  header.reserve(httpVersion.size() + 2U + static_cast<std::size_t>(nchars(statusCode)) + reason.size() + kCRLF.size() +
                 http::Date.size() + kSep.size() + date.size() + kCRLF.size() + http::ContentType.size() + kSep.size() +
                 contentType.size() + kCRLF.size() + http::ContentLength.size() + kSep.size() +
                 static_cast<std::size_t>(nchars(bodySize)) + kCRLF.size() + http::Connection.size() + kSep.size() +
                 keepAliveStr.size() + kEnd.size());

  // Trust caller to validate version (only HTTP/1.0 or HTTP/1.1 allowed upstream)
  header.append(httpVersion);
  header.push_back(' ');
  // previously appended a space after version; we already added above
  AppendIntegralToString(header, statusCode);
  header.push_back(' ');
  header.append(reason);
  header.append(kCRLF);
  header.append(http::Date);
  header.append(kSep);
  header.append(date);
  header.append(kCRLF);
  header.append(http::ContentType);
  header.append(kSep);
  header.append(contentType);
  header.append(kCRLF);
  header.append(http::ContentLength);
  header.append(kSep);
  AppendIntegralToString(header, bodySize);
  header.append(kCRLF);
  header.append(http::Connection);
  header.append(kSep);
  header.append(keepAliveStr);
  header.append(kEnd);
  return header;
}

}  // namespace aeronet
