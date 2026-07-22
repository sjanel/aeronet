#include "aeronet/http-error-build.hpp"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

RawChars BuildSimpleError(http::StatusCode status, const ConcatenatedHeaders& globalHeaders, std::string_view body) {
  const std::string_view reason = http::ReasonPhraseFor(status);

  static constexpr std::size_t kStatusLen = 3U;

  const std::size_t globalHeadersSize = globalHeaders.fullSizeWithLastSep();
  const auto nbCharsBodyLen = nchars(body.size());

  static constexpr std::string_view kHTTP11Str = "HTTP/1.1 ";
  static constexpr std::string_view kConnectionCloseStr = "\r\nconnection: close\r\n";

  // Exact allocation size
  RawChars out(
      kHTTP11Str.size() + kStatusLen + 1UL + reason.size() + http::HeaderSize(http::Date.size(), RFC7231DateStrLen) +
      kConnectionCloseStr.size() + globalHeadersSize + http::HeaderSize(http::ContentLength.size(), nbCharsBodyLen) +
      http::HeaderSize(http::ContentType.size(), http::ContentTypeTextPlain.size()) + http::CRLF.size() + body.size());

  char* pData = out.data();

  // Status line: HTTP/1.1 404 Not Found\r\n
  std::memcpy(pData, kHTTP11Str.data(), kHTTP11Str.size());
  pData += kHTTP11Str.size();

  pData = writeStatusCode(pData, status);
  *pData++ = ' ';
  pData = Append(reason, pData);

  // date: Wed, 21 Oct 2015 07:28:00 GMT
  pData = WriteCRLFDateHeader(SysClock::now(), pData);

  // connection: close
  std::memcpy(pData, kConnectionCloseStr.data(), kConnectionCloseStr.size());
  pData += kConnectionCloseStr.size();

  // Append global headers
  pData = Append(globalHeaders.fullStringWithLastSep(), pData);

  // content-length
  pData = WriteContentTypeContentLengthDoubleCRLF(http::ContentTypeTextPlain, body.size(), pData);

  // Body
  pData = Append(body, pData);

  assert(static_cast<std::size_t>(pData - out.data()) == out.capacity());

  out.setEnd(pData);

  return out;
}

}  // namespace aeronet
