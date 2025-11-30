#pragma once

#include <string_view>

namespace aeronet {

// Trim OWS (optional whitespace) per RFC7230: SP and HTAB only.
constexpr std::string_view TrimOws(std::string_view sv) noexcept {
  auto begin = sv.begin();
  auto end = sv.end();
  while (begin != end && (*begin == ' ' || *begin == '\t')) {
    ++begin;
  }
  while (begin != end) {
    --end;
    if (*end != ' ' && *end != '\t') {
      ++end;
      break;
    }
  }
  return {begin, end};
}

}  // namespace aeronet
