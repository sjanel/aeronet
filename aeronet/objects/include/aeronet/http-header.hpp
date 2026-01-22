#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>

#include "aeronet/http-constants.hpp"

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
  // Name and value are validated according to HTTP/1.1 specifications.
  // The value is trimmed.
  Header(std::string_view name, std::string_view value);

  Header(const Header &rhs);
  Header(Header &&) noexcept = default;
  Header &operator=(const Header &rhs);
  Header &operator=(Header &&) noexcept = default;

  ~Header() = default;

  // Returns the header name.
  [[nodiscard]] std::string_view name() const noexcept { return {_data.get(), _nameLen}; }

  // Returns the header value.
  [[nodiscard]] std::string_view value() const noexcept {
    return {_data.get() + _nameLen + HeaderSep.size(), _valueLen};
  }

  // Returns the raw header as "Name: Value".
  [[nodiscard]] std::string_view http1Raw() const noexcept { return {_data.get(), totalSize()}; }

  // Returns the total size of the header including name, separator, and value.
  [[nodiscard]] std::size_t totalSize() const noexcept { return HeaderSep.size() + _nameLen + _valueLen; }

  using trivially_relocatable = std::true_type;

 private:
  std::unique_ptr<char[]> _data;
  uint32_t _nameLen;
  uint32_t _valueLen;
};

// RFC 7230 §3.2: Header field values can be preceded and followed by optional whitespace (OWS).
// OWS is defined as zero or more spaces (SP) or horizontal tabs (HTAB).
constexpr bool IsHeaderWhitespace(char ch) noexcept { return ch == ' ' || ch == '\t'; }

}  // namespace aeronet::http