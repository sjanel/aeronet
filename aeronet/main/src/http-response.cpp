#include "aeronet/http-response.hpp"

#include "stringconv.hpp"

namespace aeronet {

string HttpResponse::buildHead(std::string_view httpVersion, std::string_view date, bool keepAlive,
                               std::size_t bodySize) const {
  string header;

  static constexpr std::string_view kDate = "\r\nDate: ";
  static constexpr std::string_view kContentType = "\r\nContent-Type: ";
  static constexpr std::string_view kContentLength = "\r\nContent-Length: ";
  static constexpr std::string_view kConnection = "\r\nConnection: ";
  static constexpr std::string_view kEnd = "\r\n\r\n";

  std::string_view keepAliveStr = keepAlive ? "keep-alive" : "close";

  header.reserve(httpVersion.size() + 2U + static_cast<std::size_t>(nchars(statusCode)) + reason.size() + kDate.size() +
                 date.size() + kContentType.size() + contentType.size() + kContentLength.size() +
                 static_cast<std::size_t>(nchars(bodySize)) + kConnection.size() + keepAliveStr.size() + kEnd.size());

  // Trust caller to validate version (only HTTP/1.0 or HTTP/1.1 allowed upstream)
  header.append(httpVersion);
  header.push_back(' ');
  // previously appended a space after version; we already added above
  AppendIntegralToString(header, statusCode);
  header.push_back(' ');
  header.append(reason);
  header.append(kDate);
  header.append(date);
  header.append(kContentType);
  header.append(contentType);
  header.append(kContentLength);
  AppendIntegralToString(header, bodySize);
  header.append(kConnection);
  header.append(keepAliveStr);
  header.append(kEnd);
  return header;
}

}  // namespace aeronet
