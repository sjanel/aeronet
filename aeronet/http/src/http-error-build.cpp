#include "http-error-build.hpp"

#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-status-code.hpp"
#include "raw-chars.hpp"
#include "stringconv.hpp"
#include "timedef.hpp"
#include "timestring.hpp"

namespace aeronet {

void BuildSimpleError(http::StatusCode status, TimePoint tp, RawChars &out) {
  const std::string_view reason = http::reasonPhraseFor(status);

  const auto datePos = http::HTTP11Sv.size() + 1UL + 3UL + 1UL + reason.size() + http::CRLF.size() + http::Date.size() +
                       http::HeaderSep.size();

  out.clear();
  out.ensureAvailableCapacity(datePos + kRFC7231DateStrLen + http::CRLF.size() + http::ContentLength.size() +
                              http::HeaderSep.size() + 1U + http::CRLF.size() + http::Connection.size() +
                              http::HeaderSep.size() + http::close.size() + http::DoubleCRLF.size());

  out.unchecked_append(http::HTTP11Sv);
  out.unchecked_push_back(' ');
  out.unchecked_append(std::string_view(IntegralToCharVector(status)));
  out.unchecked_push_back(' ');
  out.unchecked_append(reason);
  out.unchecked_append(http::CRLF);
  out.unchecked_append(http::Date);
  out.unchecked_append(http::HeaderSep);
  TimeToStringRFC7231(tp, out.data() + datePos);
  out.setSize(out.size() + kRFC7231DateStrLen);
  out.unchecked_append(http::CRLF);
  out.unchecked_append(http::ContentLength);
  out.unchecked_append(http::HeaderSep);
  out.unchecked_push_back('0');
  out.unchecked_append(http::CRLF);
  out.unchecked_append(http::Connection);
  out.unchecked_append(http::HeaderSep);
  out.unchecked_append(http::close);
  out.unchecked_append(http::DoubleCRLF);
}

}  // namespace aeronet
