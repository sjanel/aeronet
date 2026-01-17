#pragma once

#include <string_view>

namespace aeronet {

// Trim OWS (optional whitespace) per RFC7230: SP and HTAB only.
constexpr std::string_view TrimOws(std::string_view sv) noexcept {
  const char* begin = sv.data();
  const char* end = begin + sv.size();

  const auto isWhitespace = [](char ch) { return ch == ' ' || ch == '\t'; };

  // Fast-path: already trimmed
  if (begin == end || (!isWhitespace(begin[0]) && !isWhitespace(end[-1]))) {
    return sv;
  }

  while (begin < end && isWhitespace(begin[0])) {
    ++begin;
  }
  while (begin < end && isWhitespace(end[-1])) {
    --end;
  }

  return {begin, static_cast<size_t>(end - begin)};
}

}  // namespace aeronet
