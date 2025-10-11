#include "http-error-build.hpp"

#include <cstddef>
#include <span>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-status-code.hpp"
#include "raw-chars.hpp"
#include "stringconv.hpp"
#include "timedef.hpp"
#include "timestring.hpp"

namespace aeronet {

void BuildSimpleError(http::StatusCode status, std::span<const http::Header> globalHeaders, std::string_view reason,
                      RawChars& out) {
  const auto datePos = http::HTTP11Sv.size() + 1UL + 3UL + 1UL + reason.size() + http::CRLF.size() + http::Date.size() +
                       http::HeaderSep.size();

  std::size_t globalHeadersSize = 0;
  for (const auto& [headerKey, headerValue] : globalHeaders) {
    globalHeadersSize += http::CRLF.size() + headerKey.size() + http::HeaderSep.size() + headerValue.size();
  }

  out.clear();
  out.ensureAvailableCapacity(datePos + kRFC7231DateStrLen + http::CRLF.size() + http::ContentLength.size() +
                              http::HeaderSep.size() + 1U + http::CRLF.size() + http::Connection.size() +
                              http::HeaderSep.size() + http::close.size() + http::DoubleCRLF.size() +
                              globalHeadersSize);

  out.unchecked_append(http::HTTP11Sv);
  out.unchecked_push_back(' ');
  out.unchecked_append(std::string_view(IntegralToCharVector(status)));
  out.unchecked_push_back(' ');
  out.unchecked_append(reason);
  out.unchecked_append(http::CRLF);
  out.unchecked_append(http::Date);
  out.unchecked_append(http::HeaderSep);
  TimeToStringRFC7231(Clock::now(), out.data() + datePos);
  out.addSize(kRFC7231DateStrLen);
  out.unchecked_append(http::CRLF);
  out.unchecked_append(http::ContentLength);
  out.unchecked_append(http::HeaderSep);
  out.unchecked_push_back('0');
  out.unchecked_append(http::CRLF);
  out.unchecked_append(http::Connection);
  out.unchecked_append(http::HeaderSep);
  out.unchecked_append(http::close);
  for (const auto& [headerKey, headerValue] : globalHeaders) {
    out.unchecked_append(http::CRLF);
    out.unchecked_append(headerKey);
    out.unchecked_append(http::HeaderSep);
    out.unchecked_append(headerValue);
  }
  out.unchecked_append(http::DoubleCRLF);
}

}  // namespace aeronet
