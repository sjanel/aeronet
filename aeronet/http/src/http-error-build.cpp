#include "http-error-build.hpp"

#include <cstddef>
#include <numeric>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-status-code.hpp"
#include "raw-chars.hpp"
#include "stringconv.hpp"
#include "timedef.hpp"
#include "timestring.hpp"

namespace aeronet {

HttpResponseData BuildSimpleError(http::StatusCode status, std::span<const http::Header> globalHeaders,
                                  std::string_view reason) {
  const auto datePos = http::HTTP11Sv.size() + 1UL + 3UL + 1UL + reason.size() + http::CRLF.size() + http::Date.size() +
                       http::HeaderSep.size();

  const std::size_t globalHeadersSize = std::accumulate(
      globalHeaders.begin(), globalHeaders.end(), std::size_t{0}, [](std::size_t acc, const http::Header& header) {
        return acc + http::CRLF.size() + header.name.size() + http::HeaderSep.size() + header.value.size();
      });

  RawChars out(datePos + kRFC7231DateStrLen + http::CRLF.size() + http::ContentLength.size() + http::HeaderSep.size() +
               1U + http::CRLF.size() + http::Connection.size() + http::HeaderSep.size() + http::close.size() +
               http::DoubleCRLF.size() + globalHeadersSize);

  out.unchecked_append(http::HTTP11Sv);
  out.unchecked_push_back(' ');
  out.unchecked_append(std::string_view(IntegralToCharVector(status)));
  out.unchecked_push_back(' ');
  out.unchecked_append(reason);
  out.unchecked_append(http::CRLF);
  out.unchecked_append(http::Date);
  out.unchecked_append(http::HeaderSep);
  TimeToStringRFC7231(SysClock::now(), out.data() + datePos);
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

  return HttpResponseData(std::move(out));
}

}  // namespace aeronet
