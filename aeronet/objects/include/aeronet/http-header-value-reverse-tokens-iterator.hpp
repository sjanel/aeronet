#pragma once

#include <string_view>

namespace aeronet::http {

// Helper to iterate the comma separated tokens of a header value (Content-Encoding, Connection, Vary...) in reverse
// order.
// Given header value should be trimmed on the extremities before use.
// The tokens returned by the iterator are trimmed as well. A comma inside a quoted string does not separate tokens.
// The iterator will also yield empty tokens if there are consecutive separators or a leading separator.
// Usage:
//  http::HeaderValueReverseTokensIterator it(headerValue);
//  while (it.hasNext()) {
//    std::string_view token = it.next();
//    ...
//  }
class HeaderValueReverseTokensIterator {
 public:
  explicit HeaderValueReverseTokensIterator(std::string_view trimmedHeaderValue) noexcept
      : _first(trimmedHeaderValue.data()),
        _last(trimmedHeaderValue.data() + trimmedHeaderValue.size()),
        _emitLeadingEmpty(!trimmedHeaderValue.empty() && trimmedHeaderValue.front() == kSep) {}

  [[nodiscard]] bool hasNext() const noexcept { return _first < _last || _emitLeadingEmpty; }

  // Returns the next token, starting from the last one, and consumes it. Should only be called if hasNext() is true.
  std::string_view next() noexcept;

 private:
  static constexpr char kSep = ',';

  const char* _first;
  const char* _last;
  bool _emitLeadingEmpty;
};

}  // namespace aeronet::http
