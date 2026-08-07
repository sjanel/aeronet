#pragma once

#include <string_view>

#include "aeronet/compiler-config.hpp"
#include "aeronet/is-header-whitespace.hpp"

namespace aeronet {

// Trim OWS (optional whitespace) per RFC7230: SP and HTAB only.
AERONET_ALWAYS_INLINE constexpr std::string_view TrimOws(std::string_view sv) noexcept {
  const char* begin = sv.data();
  const char* end = begin + sv.size();

  while (begin < end && http::IsHeaderWhitespace(begin[0])) {
    ++begin;
  }
  while (begin < end && http::IsHeaderWhitespace(end[-1])) {
    --end;
  }

  return {begin, end};
}

}  // namespace aeronet
