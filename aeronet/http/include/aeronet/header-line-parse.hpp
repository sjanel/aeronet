#pragma once

#include <string_view>

#include "aeronet/http-header.hpp"

namespace aeronet::http {

// Parse a single HTTP header line (range [lineStart, lineLast)).
// Returns pair of (name, value) string_views, or empty name view on failure.
constexpr HeaderView ParseHeaderLine(const char* lineStart, const char* lineLast) {
  const char* colonPtr = lineStart;
  while (colonPtr < lineLast && *colonPtr != ':') {
    ++colonPtr;
  }

  // Value may be preceded by optional whitespace
  const char* valueFirst = colonPtr + 1;
  while (valueFirst < lineLast && IsHeaderWhitespace(*valueFirst)) {
    ++valueFirst;
  }
  const char* valueLast = lineLast;
  while (valueLast > valueFirst && IsHeaderWhitespace(*(valueLast - 1))) {
    --valueLast;
  }

  HeaderView ret{std::string_view(lineStart, colonPtr), std::string_view(valueFirst, valueLast)};

  if (colonPtr == lineLast) {
    // malformed: no colon
    ret.name = {};
  }
  return ret;
}

}  // namespace aeronet::http
