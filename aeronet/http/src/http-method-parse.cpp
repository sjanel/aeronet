#include "http-method-parse.hpp"

#include <string_view>

#include "aeronet/http-method.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/toupperlower.hpp"

namespace aeronet::http {

Method ParseMethodStr(std::string_view str) {
  switch (str.size()) {
    case 3:  // GET, PUT
      switch (toupper(str[0])) {
        case 'G':
          return CaseInsensitiveEqual(str, "GET") ? Method::GET : kMethodInvalid;
        case 'P':
          return CaseInsensitiveEqual(str, "PUT") ? Method::PUT : kMethodInvalid;
        default:
          return kMethodInvalid;
      }

    case 4:  // HEAD, POST
      switch (toupper(str[0])) {
        case 'H':
          return CaseInsensitiveEqual(str, "HEAD") ? Method::HEAD : kMethodInvalid;
        case 'P':
          return CaseInsensitiveEqual(str, "POST") ? Method::POST : kMethodInvalid;
        default:
          return kMethodInvalid;
      }

    case 5:  // TRACE, PATCH
      switch (toupper(str[0])) {
        case 'T':
          return CaseInsensitiveEqual(str, "TRACE") ? Method::TRACE : kMethodInvalid;
        case 'P':
          return CaseInsensitiveEqual(str, "PATCH") ? Method::PATCH : kMethodInvalid;
        default:
          return kMethodInvalid;
      }

    case 6:  // DELETE
      return CaseInsensitiveEqual(str, "DELETE") ? Method::DELETE : kMethodInvalid;

    case 7:  // CONNECT, OPTIONS
      switch (toupper(str[0])) {
        case 'C':
          return CaseInsensitiveEqual(str, "CONNECT") ? Method::CONNECT : kMethodInvalid;
        case 'O':
          return CaseInsensitiveEqual(str, "OPTIONS") ? Method::OPTIONS : kMethodInvalid;
        default:
          return kMethodInvalid;
      }
    default:
      return kMethodInvalid;
  }
}

}  // namespace aeronet::http