#include "aeronet/http-error.hpp"

#include <unistd.h>

#include "string.hpp"
#include "stringconv.hpp"

namespace aeronet {

bool sendSimpleError(int fd, int status, std::string_view reason, std::string_view date, bool closeConn) {
  string header;
  header.reserve(96 + reason.size());
  header.append("HTTP/1.1 ");
  AppendIntegralToString(header, status);
  header.push_back(' ');
  header.append(reason.data(), reason.size());
  header.append("\r\nDate: ");
  header.append(date.data(), date.size());
  header.append("\r\nContent-Length: 0\r\nConnection: ");
  header.append(closeConn ? "close" : "keep-alive");
  header.append("\r\n\r\n");
  ssize_t writtenBytes = ::write(fd, header.data(), header.size());
  return writtenBytes == static_cast<ssize_t>(header.size());
}

}  // namespace aeronet
