#pragma once

#include <string_view>

#include "aeronet/cctype.hpp"

namespace aeronet {

// Iterates over the tokens of a string separated by ASCII whitespace (WHATWG Infra "split on ASCII whitespace": the
// class attribute, getElementsByClassName(), DOMTokenList...), without allocating. The tokens are never empty: leading,
// trailing and repeated whitespace separates nothing.
// Usage:
//   AsciiWhitespaceTokensIterator it(classes);
//   while (it.hasNext()) {
//     const std::string_view token = it.next();
//     ...
//   }
class AsciiWhitespaceTokensIterator {
 public:
  explicit constexpr AsciiWhitespaceTokensIterator(std::string_view str) noexcept
      : _pos(str.data()), _end(str.data() + str.size()) {
    skipWhitespace();
  }

  [[nodiscard]] constexpr bool hasNext() const noexcept { return _pos != _end; }

  // Returns the next token and consumes it. Should only be called if hasNext() is true.
  constexpr std::string_view next() noexcept {
    const char* const begin = _pos;
    while (_pos != _end && !IsAsciiWhitespace(*_pos)) {
      ++_pos;
    }
    const std::string_view token(begin, _pos);
    skipWhitespace();
    return token;
  }

 private:
  constexpr void skipWhitespace() noexcept {
    while (_pos != _end && IsAsciiWhitespace(*_pos)) {
      ++_pos;
    }
  }

  const char* _pos;
  const char* _end;
};

}  // namespace aeronet
