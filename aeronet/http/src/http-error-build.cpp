#include "http-error-build.hpp"

#include <string_view>

#include "http-constants.hpp"
#include "http-status-code.hpp"
#include "raw-chars.hpp"
#include "stringconv.hpp"

namespace aeronet {

RawChars buildSimpleError(http::StatusCode status, std::string_view reason, std::string_view date, bool closeConn) {
  // If caller passed empty reason, try to supply a canonical one.
  if (reason.empty()) {
    if (auto mapped = http::reasonPhraseFor(status); !mapped.empty()) {
      reason = mapped;
    }
  }
  static constexpr std::string_view kSep = ": ";
  static constexpr std::string_view kEnd = "\r\n\r\n";

  RawChars header(http::HTTP11.size() + 1 + 3 + 1 + reason.size() + http::CRLF.size() + http::Date.size() +
                  kSep.size() + date.size() + http::CRLF.size() + http::ContentLength.size() + kSep.size() +
                  1 +  // zero length "0"
                  http::CRLF.size() + http::Connection.size() + kSep.size() +
                  (closeConn ? http::close.size() : http::keepalive.size()) + kEnd.size());

  header.append(http::HTTP11);
  header.append(' ');
  header.append(std::string_view(IntegralToCharVector(status)));
  header.append(' ');
  header.append(reason);
  header.append(http::CRLF);
  header.append(http::Date);
  header.append(kSep);
  header.append(date);
  header.append(http::CRLF);
  header.append(http::ContentLength);
  header.append(kSep);
  header.append('0');
  header.append(http::CRLF);
  header.append(http::Connection);
  header.append(kSep);
  header.append(closeConn ? http::close : http::keepalive);
  header.append(kEnd);

  return header;
}

}  // namespace aeronet
