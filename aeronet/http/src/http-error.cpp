#include "http-error.hpp"

#include <unistd.h>

#include "http-constants.hpp"
#include "string.hpp"
#include "stringconv.hpp"

namespace aeronet {

bool sendSimpleError(int fd, int status, std::string_view reason, std::string_view date, bool closeConn) {
  // If caller passed empty reason, try to supply a canonical one.
  if (reason.empty()) {
    if (auto mapped = http::reasonPhraseFor(status); !mapped.empty()) {
      reason = mapped;
    }
  }
  static constexpr std::string_view kCRLF = "\r\n";
  static constexpr std::string_view kSep = ": ";
  static constexpr std::string_view kEnd = "\r\n\r\n";

  // Pre-compute approximate size.
  string header;
  header.reserve(http::HTTP11.size() + 1 + 3 + 1 + reason.size() + kCRLF.size() + http::Date.size() + kSep.size() +
                 date.size() + kCRLF.size() + http::ContentLength.size() + kSep.size() + 1 +  // zero length "0"
                 kCRLF.size() + http::Connection.size() + kSep.size() +
                 (closeConn ? http::close.size() : http::keepalive.size()) + kEnd.size());

  header.append(http::HTTP11);
  header.push_back(' ');
  AppendIntegralToString(header, status);
  header.push_back(' ');
  header.append(reason);
  header.append(kCRLF);
  header.append(http::Date);
  header.append(kSep);
  header.append(date);
  header.append(kCRLF);
  header.append(http::ContentLength);
  header.append(kSep);
  header.push_back('0');
  header.append(kCRLF);
  header.append(http::Connection);
  header.append(kSep);
  header.append(closeConn ? http::close : http::keepalive);
  header.append(kEnd);

  ssize_t writtenBytes = ::write(fd, header.data(), header.size());
  return writtenBytes == static_cast<ssize_t>(header.size());
}

}  // namespace aeronet
