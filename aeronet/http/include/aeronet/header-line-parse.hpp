#pragma once

#include <string_view>

#include "aeronet/compiler-config.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet::http {

// Parse a single HTTP header line (range [lineStart, lineLast)).
// Returns pair of (name, value) string_views, or empty name view on failure.
// Both production callers invoke this from their header-processing loops, so duplicating this small
// hot-path primitive avoids a call per header with negligible code-size cost.
AERONET_ALWAYS_INLINE constexpr HeaderView ParseHeaderLine(const char* lineStart, const char* lineLast) {
  const std::string_view line(lineStart, lineLast);
  const auto colonPos = line.find(':');
  if (colonPos == std::string_view::npos) [[unlikely]] {
    return HeaderView{};
  }
  return HeaderView{line.substr(0, colonPos), TrimOws(line.substr(colonPos + 1))};
}

}  // namespace aeronet::http
