#pragma once

#include <string_view>

#include "aeronet/compiler-config.hpp"
#include "aeronet/find-char.hpp"
#include "aeronet/http-header-view.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet::http {

// Parse a single HTTP header line (range [lineStart, lineLast)).
// Returns pair of (name, value) string_views, or empty name view on failure.
// Both production callers invoke this from their header-processing loops, so duplicating this small
// hot-path primitive avoids a call per header with negligible code-size cost. Made of pointers and memchr only: the
// string_view::find() and substr() calls it replaces are inlined or not depending on the inlining budget left in the
// translation unit of the caller, which any change of the parsing of a head can exhaust.
AERONET_ALWAYS_INLINE HeaderView ParseHeaderLineTrimValue(const char* lineStart, const char* lineLast) {
  const char* pColon = FindChar(lineStart, lineLast, ':');
  if (pColon == lineLast) [[unlikely]] {
    return {};
  }
  return {std::string_view(lineStart, pColon), TrimOws(std::string_view(pColon + 1, lineLast))};
}

}  // namespace aeronet::http
