#pragma once

#include <string>
#include <string_view>

namespace aeronet::http {

struct Header {
  std::string name;
  std::string value;
};

struct HeaderView {
  std::string_view name;
  std::string_view value;
};

// RFC 7230 §3.2: Header field values can be preceded and followed by optional whitespace (OWS).
// OWS is defined as zero or more spaces (SP) or horizontal tabs (HTAB).
constexpr bool IsHeaderWhitespace(char ch) noexcept { return ch == ' ' || ch == '\t'; }

}  // namespace aeronet::http