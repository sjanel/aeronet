#include "aeronet/http-response.hpp"

#include "stringconv.hpp"

namespace aeronet {

string HttpResponse::buildHead(std::string_view date, bool keepAlive, std::size_t bodySize) const {
  string header;
  header.reserve(128 + bodySize);
  header.append("HTTP/1.1 ");
  AppendIntegralToString(header, statusCode);
  header.push_back(' ');
  header.append(reason.data(), reason.size());
  header.append("\r\nDate: ");
  header.append(date.data(), date.size());
  header.append("\r\nContent-Type: ");
  header.append(contentType.data(), contentType.size());
  header.append("\r\nContent-Length: ");
  AppendIntegralToString(header, bodySize);
  header.append("\r\nConnection: ");
  header.append(keepAlive ? "keep-alive" : "close");
  header.append("\r\n\r\n");
  return header;
}

}  // namespace aeronet
