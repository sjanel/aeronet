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

// Whether more input can still make [first, last), received from the start of a line of an HTTP head or trailer
// section, the start of valid lines: every CR is followed by a LF and every LF preceded by a CR (RFC 9112 §2.2: aeronet
// rejects a bare CR or LF), but a last CR, whose LF may come in the next read. When SearchCRLF() does not find the
// CRLF ending a line, this tells whether to wait for the rest of it: a line that can no longer be valid is answered
// right away, not once its head times out. Out of line: only needed when a line is incomplete or malformed.
[[nodiscard]] bool CanStillBeValidLines(const char* first, const char* last) noexcept;

}  // namespace aeronet
