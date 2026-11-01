#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/is-header-whitespace.hpp"

namespace aeronet::http {

// Helper to iterate Content-Encoding header values in reverse order.
// Given header value should be trimmed on the extremities before use.
// The tokens returned by the iterator are trimmed as well.
// The iterator will also yield empty tokens if there are consecutive separators or a leading separator.
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