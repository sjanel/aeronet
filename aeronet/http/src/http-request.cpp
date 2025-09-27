#include "aeronet/http-request.hpp"

#include <string_view>

#include "http-constants.hpp"
#include "string-equal-ignore-case.hpp"
#include "toupperlower.hpp"
namespace aeronet {

std::string_view HttpRequest::findHeader(std::string_view key) const {
  // Fast path: exact case match first (common)
  if (auto it = headers.find(key); it != headers.end()) {
    return it->second;
  }
  for (const auto& kv : headers) {
    if (CaseInsensitiveEqual(kv.first, key)) {
      return kv.second;
    }
  }
  return {};
}

[[nodiscard]] bool HttpRequest::wantClose() const {
  std::string_view connVal = findHeader(http::Connection);
  if (connVal.size() == http::close.size()) {
    for (std::size_t iChar = 0; iChar < http::close.size(); ++iChar) {
      const char lhs = tolower(connVal[iChar]);
      const char rhs = static_cast<char>(http::close[iChar]);
      if (lhs != rhs) {
        return false;
      }
    }
    return true;
  }
  return false;
}

}  // namespace aeronet