#include "aeronet/http-header-value-reverse-tokens-iterator.hpp"

#include <cstddef>
#include <string_view>

#include "aeronet/is-header-whitespace.hpp"

namespace aeronet::http {

std::string_view HeaderValueReverseTokensIterator::next() noexcept {
  // Final leading empty field
  if (_first == _last) {
    _emitLeadingEmpty = false;
    return {};
  }

  const char* pos = _last - 1;
  const char* pTokenEnd = _last;

  bool inQuotes = false;

  while (pos >= _first) {
    char ch = *pos;

    if (ch == '"') {
      // Check if escaped (count backslashes before)
      const char* pPrev = pos - 1;
      std::size_t backslashes = 0;
      while (pPrev >= _first && *pPrev == '\\') {
        ++backslashes;
        --pPrev;
      }
      if ((backslashes & 1U) == 0U) {
        inQuotes = !inQuotes;
      }
    } else if (!inQuotes && ch == kSep) {
      break;
    }

    --pos;
  }

  const char* pTokenBeg = pos + 1;

  // Trim OWS inside token bounds (fast, bounded)
  while (pTokenBeg < pTokenEnd && IsHeaderWhitespace(*pTokenBeg)) {
    ++pTokenBeg;
  }
  while (pTokenBeg < pTokenEnd && IsHeaderWhitespace(*(pTokenEnd - 1))) {
    --pTokenEnd;
  }

  // Consume separator + OWS
  if (pos >= _first) {
    --pos;  // skip Sep
    while (pos >= _first && IsHeaderWhitespace(*pos)) {
      --pos;
    }
  }

  _last = pos + 1;
  return {pTokenBeg, pTokenEnd};
}

}  // namespace aeronet::http
