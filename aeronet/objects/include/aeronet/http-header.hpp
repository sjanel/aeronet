#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "aeronet/is-header-whitespace.hpp"

namespace aeronet::http {

struct HeaderView {
  std::string_view name;
  std::string_view value;
};

// Simple class holding a header name/value pair. The name and value are stored in a single contiguous dynamically
// allocated buffer, which is owned by the Header object. The name and value are stored in the buffer as a single
// string, with the name first, followed by the value. The name and value can be accessed via the name() and value()
// methods, which return string_views into the buffer. The size() method returns the total size of the header (name +
// value).
class Header {
 public:
  // Create an empty header entry (empty name and value). The buffer is not allocated until a non-empty name/value is
  // set.
  Header() noexcept = default;

  // Create a header entry with the specified name and value.
  Header(std::string_view name, std::string_view value);

  // Same as above, but by stealing the buffer of 'rhs'.
  // If it is smaller than the new name/value, the buffer will be reallocated.
  Header(Header&& rhs, std::string_view name, std::string_view value);

  // Header supports both move and copy semantics.
  Header(const Header&);
  Header(Header&& rhs) noexcept;
  Header& operator=(const Header&);
  Header& operator=(Header&& rhs) noexcept;

  ~Header();

  // Returns true if the header is empty (no name/value).
  [[nodiscard]] bool empty() const noexcept { return _pData == nullptr; }

  // Total size of the header, which is name.size() + value.size().
  [[nodiscard]] std::size_t size() const noexcept { return static_cast<std::size_t>(_nameLength) + _valueLength; }

  // Returns the name of the header as a string_view.
  [[nodiscard]] std::string_view name() const noexcept { return {_pData, _nameLength}; }

  // Returns the value of the header as a string_view.
  [[nodiscard]] std::string_view value() const noexcept { return {_pData + _nameLength, _valueLength}; }

  using trivially_relocatable = std::true_type;

 private:
  char* _pData{};
  uint32_t _nameLength{};
  uint32_t _valueLength{};
};

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

    const char* pos = _last - 1;
    const char* tokenEnd = _last;

    bool inQuotes = false;

    while (pos >= _first) {
      char ch = *pos;

      if (ch == '"') {
        // Check if escaped (count backslashes before)
        const char* prev = pos - 1;
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

    const char* tokenBegin = pos + 1;

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
  const char* _first;
  const char* _last;
  bool _emitLeadingEmpty;
};

}  // namespace aeronet::http