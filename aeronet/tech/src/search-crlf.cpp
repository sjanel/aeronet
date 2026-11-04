#include "aeronet/search-crlf.hpp"

#include <cstddef>
#include <cstring>

#include "aeronet/http-constants.hpp"

namespace aeronet {

bool CanStillBeValidLines(const char* first, const char* last) noexcept {
  while (true) {
    // The first CR, or last if there is none but the last byte: no LF before it is preceded by a CR.
    const char* pCR = SearchCRLF(first, last);
    if (std::memchr(first, '\n', static_cast<std::size_t>(pCR - first)) != nullptr) {
      return false;
    }
    if (pCR == last) {
      return true;
    }
    if (pCR[1] != '\n') {
      return false;
    }
    first = pCR + http::CRLF.size();
  }
}

}  // namespace aeronet
