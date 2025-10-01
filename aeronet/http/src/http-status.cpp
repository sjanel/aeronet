#include <string_view>

#include "http-constants.hpp"
#include "http-status-build.hpp"
#include "http-status-code.hpp"
#include "nchars.hpp"
#include "raw-chars.hpp"
#include "stringconv.hpp"

namespace aeronet::http {

RawChars buildStatusLine(http::StatusCode code, std::string_view reason) {
  RawChars ret(std::max<std::size_t>(
      96UL, HTTP11Sv.size() + 2UL + static_cast<std::size_t>(nchars(code)) + reason.size() + CRLF.size()));

  ret.unchecked_append(HTTP11Sv);
  ret.unchecked_push_back(' ');
  ret.unchecked_append(std::string_view(IntegralToCharVector(code)));
  ret.unchecked_push_back(' ');
  ret.unchecked_append(reason);
  ret.unchecked_append(CRLF);

  return ret;
}

}  // namespace aeronet::http