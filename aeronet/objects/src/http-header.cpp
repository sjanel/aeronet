#include "aeronet/http-header.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/tchars.hpp"

namespace aeronet::http {

Header::Header(std::string_view name, std::string_view value) : _colonPos(static_cast<uint32_t>(name.size())) {
  value = TrimOws(value);
  std::size_t totalSize = name.size() + HeaderSep.size() + value.size();
  if (totalSize > static_cast<std::size_t>(std::numeric_limits<decltype(_data)::size_type>::max())) {
    throw std::invalid_argument("HTTP header size exceeds maximum allowed");
  }
  if (!IsValidHeaderName(name)) {
    throw std::invalid_argument("HTTP header name is invalid");
  }
  if (!IsValidHeaderValue(value)) {
    throw std::invalid_argument("HTTP header value is invalid");
  }
  _data.reserve(static_cast<decltype(_data)::size_type>(totalSize));
  _data.unchecked_append(name);
  _data.unchecked_append(HeaderSep);
  _data.unchecked_append(value);
}

bool IsValidHeaderName(std::string_view name) noexcept {
  return !name.empty() && std::ranges::all_of(name, [](char ch) { return is_tchar(ch); });
}

bool IsValidHeaderValue(std::string_view value) noexcept {
  return std::ranges::all_of(value, [](unsigned char ch) {
    if (ch == '\r' || ch == '\n') {
      return false;
    }
    if (ch == '\t') {
      return true;
    }
    // Visible ASCII characters
    return ch >= 0x20 && ch <= 0x7E;
  });
}

}  // namespace aeronet::http