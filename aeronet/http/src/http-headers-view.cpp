#include "aeronet/http-headers-view.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>

#include "aeronet/http-constants.hpp"

namespace aeronet {

HeadersView::iterator::iterator(const char* beg, const char* end) noexcept : _cur(beg), _end(end) {
  if (_cur != _end) {
    setLen();
  }
}

void HeadersView::iterator::setLen() {
  const char* colonPtr = std::search(_cur, _end, http::HeaderSep.begin(), http::HeaderSep.end());
  assert(colonPtr != _end);  // should not happen in well-formed headers
  const char* begValue = colonPtr + http::HeaderSep.size();

  _nameLen = static_cast<uint32_t>(colonPtr - _cur);
  _valueLen = static_cast<uint32_t>(std::search(begValue, _end, http::CRLF.begin(), http::CRLF.end()) - begValue);
}

}  // namespace aeronet