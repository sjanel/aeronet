#pragma once

#include <string_view>

#include "aeronet/http-header.hpp"

namespace aeronet::http {

// Parse a single HTTP header line (range [lineStart, lineLast)).
// Returns pair of (name, value) string_views, or empty name view on failure.
inline HeaderView parseHeaderLine(const char* lineStart, const char* lineLast) {
  const auto* colonPtr = lineStart;
  while (colonPtr < lineLast && *colonPtr != ':') {
    ++colonPtr;
  }

  // Value may be preceded by optional whitespace
  const auto* valueFirst = colonPtr + 1;
  while (valueFirst < lineLast && IsHeaderWhitespace(*valueFirst)) {
    ++valueFirst;
  }
  const auto* valueLast = lineLast;
  if (valueLast > lineStart && *(valueLast - 1) == '\r') {
    --valueLast;
  }
  while (valueLast > valueFirst && IsHeaderWhitespace(*(valueLast - 1))) {
    --valueLast;
  }

  HeaderView ret{std::string_view(lineStart, colonPtr),
                 std::string_view(valueFirst, static_cast<std::size_t>(valueLast - valueFirst))};

  if (colonPtr == lineLast) {
    ret.name = {};  // malformed: no colon
  }
  return ret;
}

}  // namespace aeronet::http
