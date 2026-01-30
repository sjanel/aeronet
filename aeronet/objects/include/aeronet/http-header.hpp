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

// Helper to iterate Content-Encoding header values in reverse order.
// Given header value should be trimmed on the extremities before use.
// The iterator will also yield empty tokens if there are consecutive separators or
// a leading separator.
// Usage:
//  http::HeaderValueReverseTokensIterator<','> it(headerValue);
//  while (it.hasNext()) {
//    std::string_view token = it.next();
//    ...
//  }
template <char Sep>
class HeaderValueReverseTokensIterator {
 public:
  explicit HeaderValueReverseTokensIterator(std::string_view trimmedHeaderValue)
      : _first(trimmedHeaderValue.data()),
        _last(trimmedHeaderValue.data() + trimmedHeaderValue.size()),
        _emitLeadingEmpty(!trimmedHeaderValue.empty() && trimmedHeaderValue.front() == Sep) {}

  [[nodiscard]] bool hasNext() const noexcept { return _first < _last || _emitLeadingEmpty; }

  std::string_view next() {
    // Final leading empty field
    if (_first == _last) {
      _emitLeadingEmpty = false;
      return {};
    }

    const char *pos = _last - 1;
    const char *tokenEnd = _last;

    bool inQuotes = false;

    while (pos >= _first) {
      char ch = *pos;

      if (ch == '"') {
        // Check if escaped (count backslashes before)
        const char *prev = pos - 1;
        size_t backslashes = 0;
        while (prev >= _first && *prev == '\\') {
          ++backslashes;
          --prev;
        }
        if ((backslashes & 1U) == 0U) {
          inQuotes = !inQuotes;
        }
      } else if (!inQuotes && ch == Sep) {
        break;
      }

      --pos;
    }

    const char *tokenBegin = pos + 1;

    // Trim OWS inside token bounds (fast, bounded)
    while (tokenBegin < tokenEnd && IsHeaderWhitespace(*tokenBegin)) {
      ++tokenBegin;
    }
    while (tokenBegin < tokenEnd && IsHeaderWhitespace(*(tokenEnd - 1))) {
      --tokenEnd;
    }

    // Consume separator + OWS
    if (pos >= _first) {
      --pos;  // skip Sep
      while (pos >= _first && IsHeaderWhitespace(*pos)) {
        --pos;
      }
    }

    _last = pos + 1;
    return {tokenBegin, tokenEnd};
  }

 private:
  const char *_first;
  const char *_last;
  bool _emitLeadingEmpty;
};

}  // namespace aeronet::http