#include "aeronet/http-helpers.hpp"

#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

RawChars MakeHttp1HeaderLine(std::string_view name, std::string_view value, bool withCRLF) {
  RawChars line(name.size() + value.size() + http::HeaderSep.size() + (withCRLF ? http::CRLF.size() : 0));
  line.unchecked_append(name);
  line.unchecked_append(http::HeaderSep);
  line.unchecked_append(value);
  if (withCRLF) {
    line.unchecked_append(http::CRLF);
  }
  return line;
}

RawChars BuildRawHttp11(std::string_view method, std::string_view target, std::string_view extraHeaders,
                        std::string_view body) {
  static constexpr std::string_view kHttp11 = " HTTP/1.1\r\n";
  RawChars raw(method.size() + 1 + target.size() + kHttp11.size() + extraHeaders.size() + http::CRLF.size() +
               body.size());
  raw.unchecked_append(method);
  raw.unchecked_push_back(' ');
  raw.unchecked_append(target);
  raw.unchecked_append(kHttp11);
  raw.unchecked_append(extraHeaders);
  raw.unchecked_append(http::CRLF);
  raw.unchecked_append(body);
  return raw;
}

}  // namespace aeronet