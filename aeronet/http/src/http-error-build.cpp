#include "aeronet/http-error-build.hpp"

#include <cstddef>
#include <string_view>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet {

RawChars BuildSimpleError(http::StatusCode status, const ConcatenatedHeaders& globalHeaders, std::string_view reason) {
  if (reason.empty()) {
    reason = http::reasonPhraseFor(status);
  }

  const auto datePos = http::HTTP11Sv.size() + 1UL + 3UL + 1UL + reason.size() + http::CRLF.size() + http::Date.size() +
                       http::HeaderSep.size();

  const std::size_t globalHeadersSize = globalHeaders.fullSizeWithLastSep();

  RawChars out(datePos + kRFC7231DateStrLen + http::CRLF.size() + http::ContentLength.size() + http::HeaderSep.size() +
               1U + http::CRLF.size() + http::Connection.size() + http::HeaderSep.size() + http::close.size() +
               http::DoubleCRLF.size() + globalHeadersSize);

  // Status line: HTTP/1.1 404 Not Found\r\n
  out.unchecked_append(http::HTTP11Sv);
  out.unchecked_push_back(' ');
  out.unchecked_append(std::string_view(IntegralToCharVector(status)));
  out.unchecked_push_back(' ');
  out.unchecked_append(reason);
  out.unchecked_append(http::CRLF);

  // Date: Wed, 21 Oct 2015 07:28:00 GMT
  out.unchecked_append(http::Date);
  out.unchecked_append(http::HeaderSep);
  TimeToStringRFC7231(SysClock::now(), out.data() + datePos);
  out.addSize(kRFC7231DateStrLen);
  out.unchecked_append(http::CRLF);

  // Content-Length: 0
  out.unchecked_append(http::ContentLength);
  out.unchecked_append(http::HeaderSep);
  out.unchecked_push_back('0');
  out.unchecked_append(http::CRLF);

  // Connection: close
  out.unchecked_append(http::Connection);
  out.unchecked_append(http::HeaderSep);
  out.unchecked_append(http::close);
  out.unchecked_append(http::CRLF);

  // Append global headers
  out.unchecked_append(globalHeaders.fullStringWithLastSep());

  // End of headers, and no body
  out.unchecked_append(http::CRLF);

  return out;
}

}  // namespace aeronet
