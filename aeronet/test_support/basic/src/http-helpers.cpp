#include "aeronet/http-helpers.hpp"

#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

RawChars MakeHttp1HeaderLine(std::string_view name, std::string_view value, bool withCRLF) {
  RawChars line(name.size() + value.size() + http::HeaderSep.size() + (withCRLF ? http::CRLF.size() : 0));
  char* ptr = line.data();

  ptr = Append(name, ptr);
  ptr = Append(http::HeaderSep, ptr);
  ptr = Append(value, ptr);
  if (withCRLF) {
    ptr = Append(http::CRLF, ptr);
  }

  line.setEnd(ptr);

  return line;
}

RawChars BuildRawHttp11(std::string_view method, std::string_view target, std::string_view extraHeaders,
                        std::string_view body) {
  static constexpr std::string_view kHttp11 = " HTTP/1.1\r\n";
  RawChars raw(method.size() + 1 + target.size() + kHttp11.size() + extraHeaders.size() + http::CRLF.size() +
               body.size());
  char* ptr = raw.data();

  ptr = Append(method, ptr);
  *ptr++ = ' ';
  ptr = Append(target, ptr);
  ptr = Append(kHttp11, ptr);
  ptr = Append(extraHeaders, ptr);
  ptr = Append(http::CRLF, ptr);
  ptr = Append(body, ptr);

  raw.setEnd(ptr);

  return raw;
}

}  // namespace aeronet