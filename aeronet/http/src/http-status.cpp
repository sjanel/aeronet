#include <string_view>

#include "http-constants.hpp"
#include "http-status-build.hpp"
#include "http-status-code.hpp"
#include "raw-chars.hpp"
#include "stringconv.hpp"

namespace aeronet::http {

RawChars buildStatusLine(http::StatusCode code, std::string_view reason) {
  RawChars ret(96);

  ret.append(http::HTTP11);
  ret.append(' ');
  ret.append(std::string_view(IntegralToCharVector(code)));
  ret.append(' ');
  ret.append(reason);
  ret.append(http::CRLF);

  return ret;
}

}  // namespace aeronet::http