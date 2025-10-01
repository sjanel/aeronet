#include "http-error-build.hpp"

#include <string_view>

#include "http-constants.hpp"
#include "http-status-code.hpp"
#include "raw-chars.hpp"
#include "stringconv.hpp"

namespace aeronet {

RawChars BuildSimpleError(http::StatusCode status, std::string_view date, bool closeConnection) {
  std::string_view reason = http::reasonPhraseFor(status);

  RawChars header(http::HTTP11Sv.size() + 1UL + 3UL + 1UL + reason.size() + (3UL * http::CRLF.size()) +
                  http::Date.size() + (3UL * http::HeaderSep.size()) + date.size() + http::ContentLength.size() + 1 +
                  http::Connection.size() + (closeConnection ? http::close.size() : http::keepalive.size()) +
                  http::DoubleCRLF.size());

  header.unchecked_append(http::HTTP11Sv);
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
  header.unchecked_append(closeConnection ? http::close : http::keepalive);
  header.unchecked_append(http::DoubleCRLF);

  return header;
}

}  // namespace aeronet
