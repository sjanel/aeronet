#include "aeronet/http-request.hpp"

#include <string_view>

#include "string-equal-ignore-case.hpp"

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

}  // namespace aeronet