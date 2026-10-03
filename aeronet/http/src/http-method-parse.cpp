#include "http-method-parse.hpp"

#include <string_view>

#include "aeronet/http-method.hpp"

namespace aeronet::http {

Method ParseMethodStr(std::string_view str) {
  switch (str.size()) {
    case 3:  // GET, PUT
      switch (str[0]) {
        case 'G':
          return str == "GET" ? Method::GET : kMethodInvalid;
        case 'P':
          return str == "PUT" ? Method::PUT : kMethodInvalid;
        default:
          return kMethodInvalid;
      }

    case 4:  // HEAD, POST
      switch (str[0]) {
        case 'H':
          return str == "HEAD" ? Method::HEAD : kMethodInvalid;
        case 'P':
          return str == "POST" ? Method::POST : kMethodInvalid;
        default:
          return kMethodInvalid;
      }

    case 5:  // TRACE, PATCH
      switch (str[0]) {
        case 'T':
          return str == "TRACE" ? Method::TRACE : kMethodInvalid;
        case 'P':
          return str == "PATCH" ? Method::PATCH : kMethodInvalid;
        default:
          return kMethodInvalid;
      }

    case 6:  // DELETE
      return str == "DELETE" ? Method::DELETE : kMethodInvalid;

    case 7:  // CONNECT, OPTIONS
      switch (str[0]) {
        case 'C':
          return str == "CONNECT" ? Method::CONNECT : kMethodInvalid;
        case 'O':
          return str == "OPTIONS" ? Method::OPTIONS : kMethodInvalid;
        default:
          return kMethodInvalid;
      }
    default:
      return kMethodInvalid;
  }
}

}  // namespace aeronet::http