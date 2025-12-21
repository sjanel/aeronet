#pragma once

#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::http {

struct HeaderView {
  std::string_view name;
  std::string_view value;
};

// Represents a single HTTP header field.
// The name and value are validated upon construction.
class Header {
 public:
  // Constructs a Header with the given name and value.
  // The value is trimmed.
  explicit Header(std::string_view name, std::string_view value);

  // Returns the header name.
  [[nodiscard]] std::string_view name() const noexcept { return {_data.data(), _colonPos}; }

  // Returns the header value.
  [[nodiscard]] std::string_view value() const noexcept {
    return {_data.begin() + _colonPos + HeaderSep.size(), _data.end()};
  }

  // Returns the raw header as "Name: Value".
  [[nodiscard]] std::string_view raw() const noexcept { return _data; }

 private:
  RawChars32 _data;
  uint32_t _colonPos;
};

// RFC 7230 §3.2: Header field values can be preceded and followed by optional whitespace (OWS).
// OWS is defined as zero or more spaces (SP) or horizontal tabs (HTAB).
constexpr bool IsHeaderWhitespace(char ch) noexcept { return ch == ' ' || ch == '\t'; }

// Validates that a header name consists only of tchar characters as per RFC 7230 §3.2.6.
bool IsValidHeaderName(std::string_view name) noexcept;

// Validates that a header value does not contain any invalid characters for HTTP 1, 2 and 3.
// Specifically, it must not contain CR or LF characters, but may contain HTAB and visible ASCII characters.
// The empty value is allowed.
bool IsValidHeaderValue(std::string_view value) noexcept;

}  // namespace aeronet::http