#include "http-error-build.hpp"

#include <string_view>

#include "http-constants.hpp"
#include "http-status-code.hpp"
#include "raw-chars.hpp"
#include "stringconv.hpp"

namespace aeronet {

RawChars BuildSimpleError(http::StatusCode status, std::string_view date, bool closeConn) {
  std::string_view reason = http::reasonPhraseFor(status);

  static constexpr std::string_view kEnd = "\r\n\r\n";

  RawChars header(http::HTTP11.size() + 1UL + 3UL + 1UL + reason.size() + (3UL * http::CRLF.size()) +
                  http::Date.size() + (3UL * http::HeaderSep.size()) + date.size() + http::ContentLength.size() + 1 +
                  http::Connection.size() + (closeConn ? http::close.size() : http::keepalive.size()) + kEnd.size());

  header.unchecked_append(http::HTTP11);
  header.unchecked_push_back(' ');
  header.unchecked_append(std::string_view(IntegralToCharVector(status)));
  header.unchecked_push_back(' ');
  header.unchecked_append(reason);
  header.unchecked_append(http::CRLF);
  header.unchecked_append(http::Date);
  header.unchecked_append(http::HeaderSep);
  header.unchecked_append(date);
  header.unchecked_append(http::CRLF);
  header.unchecked_append(http::ContentLength);
  header.unchecked_append(http::HeaderSep);
  header.unchecked_push_back('0');
  header.unchecked_append(http::CRLF);
  header.unchecked_append(http::Connection);
  header.unchecked_append(http::HeaderSep);
  header.unchecked_append(closeConn ? http::close : http::keepalive);
  header.unchecked_append(kEnd);

  return header;
}

}  // namespace aeronet
