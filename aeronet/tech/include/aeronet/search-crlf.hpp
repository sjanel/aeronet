#pragma once

#include <cstddef>
#include <cstring>

namespace aeronet {

// Inspect the first CR in a streaming HTTP line. A trailing CR may be incomplete; a CR followed by anything other
// than LF is malformed and must not be skipped in search of a later CRLF.
// Returns a pointer to the CR if a possible CRLF is found (next char should be checked as a '\n'), last if more data is
// needed.
[[nodiscard]] auto* SearchCRLF(auto* first, auto* last) noexcept {
  static_assert(sizeof(*first) == 1, "SearchCRLF only works on byte ranges");
  first = static_cast<decltype(first)>(std::memchr(first, '\r', static_cast<std::size_t>(last - first)));
  if (first == nullptr || first + 1 == last) {
    return last;
  }
  return first;
}

}  // namespace aeronet
