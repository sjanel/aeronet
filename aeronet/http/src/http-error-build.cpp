#include "aeronet/http-error-build.hpp"

#include <charconv>
#include <cstddef>
#include <string_view>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet {

RawChars BuildSimpleError(http::StatusCode status, const ConcatenatedHeaders& globalHeaders, std::string_view body) {
  const std::string_view reason = http::ReasonPhraseFor(status);

  static constexpr std::size_t kStatusLen = 3U;

  const std::size_t globalHeadersSize = globalHeaders.fullSizeWithLastSep();
  const auto nbCharsBodyLen = nchars(body.size());

  // Exact allocation size
  RawChars out(http::HTTP11Sv.size() + 1UL + kStatusLen + 1UL + reason.size() + http::CRLF.size() + http::Date.size() +
               http::HeaderSep.size() + RFC7231DateStrLen + http::CRLF.size() + http::ContentLength.size() +
               http::HeaderSep.size() + nbCharsBodyLen + http::CRLF.size() + http::Connection.size() +
               http::HeaderSep.size() + http::close.size() + http::DoubleCRLF.size() + globalHeadersSize + body.size());

  char* ptr = out.data();

  // Status line: HTTP/1.1 404 Not Found\r\n
  ptr = Append(http::HTTP11Sv, ptr);
  *ptr++ = ' ';
  ptr = write3(ptr, status);
  *ptr++ = ' ';
  ptr = Append(reason, ptr);
  ptr = Append(http::CRLF, ptr);

  // date: Wed, 21 Oct 2015 07:28:00 GMT
  ptr = Append(http::Date, ptr);
  ptr = Append(http::HeaderSep, ptr);
  ptr = TimeToStringRFC7231(SysClock::now(), ptr);
  ptr = Append(http::CRLF, ptr);

  // content-length
  ptr = Append(http::ContentLength, ptr);
  ptr = Append(http::HeaderSep, ptr);
  ptr = std::to_chars(ptr, ptr + nbCharsBodyLen, body.size()).ptr;
  ptr = Append(http::CRLF, ptr);

  // connection: close
  ptr = Append(http::Connection, ptr);
  ptr = Append(http::HeaderSep, ptr);
  ptr = Append(http::close, ptr);
  ptr = Append(http::CRLF, ptr);

  // Append global headers
  ptr = Append(globalHeaders.fullStringWithLastSep(), ptr);

  // End of headers
  ptr = Append(http::CRLF, ptr);

  // Body
  ptr = Append(body, ptr);

  out.setSize(static_cast<std::size_t>(ptr - out.data()));

  return out;
}

}  // namespace aeronet
