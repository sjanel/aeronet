#include "http-method-parse.hpp"

#include <optional>
#include <string_view>

#include "aeronet/http-method.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/toupperlower.hpp"

namespace aeronet::http {

std::optional<Method> MethodStrToOptEnum(std::string_view str) {
  switch (str.size()) {
    case 3:  // GET, PUT
      switch (toupper(str[0])) {
        case 'G':
          return CaseInsensitiveEqual(str, "GET") ? std::optional<Method>(Method::GET) : std::nullopt;
        case 'P':
          return CaseInsensitiveEqual(str, "PUT") ? std::optional<Method>(Method::PUT) : std::nullopt;
        default:
          return std::nullopt;
      }

    case 4:  // HEAD, POST
      switch (toupper(str[0])) {
        case 'H':
          return CaseInsensitiveEqual(str, "HEAD") ? std::optional<Method>(Method::HEAD) : std::nullopt;
        case 'P':
          return CaseInsensitiveEqual(str, "POST") ? std::optional<Method>(Method::POST) : std::nullopt;
        default:
          return std::nullopt;
      }

    case 5:  // TRACE, PATCH
      switch (toupper(str[0])) {
        case 'T':
          return CaseInsensitiveEqual(str, "TRACE") ? std::optional<Method>(Method::TRACE) : std::nullopt;
        case 'P':
          return CaseInsensitiveEqual(str, "PATCH") ? std::optional<Method>(Method::PATCH) : std::nullopt;
        default:
          return std::nullopt;
      }

    case 6:  // DELETE
      return CaseInsensitiveEqual(str, "DELETE") ? std::optional<Method>(Method::DELETE) : std::nullopt;

    case 7:  // CONNECT, OPTIONS
      switch (toupper(str[0])) {
        case 'C':
          return CaseInsensitiveEqual(str, "CONNECT") ? std::optional<Method>(Method::CONNECT) : std::nullopt;
        case 'O':
          return CaseInsensitiveEqual(str, "OPTIONS") ? std::optional<Method>(Method::OPTIONS) : std::nullopt;
        default:
          return std::nullopt;
      }
    default:
      return std::nullopt;
  }
}

}  // namespace aeronet::http