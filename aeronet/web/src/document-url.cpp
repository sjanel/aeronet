#include "document-url.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/memory-utils-sv.hpp"
#include "web-url.hpp"

namespace aeronet::internal {

bool DocumentUrl::isSameOrigin(std::string_view url) const noexcept {
  // The authority of a serialized http or https URL is followed by its path.
  return url.size() > _originSize && url.starts_with(origin()) && url[_originSize] == '/';
}

void DocumentUrl::assign(std::string_view url) {
  const UrlParts parts = SplitUrl(url);
  assert(parts.host.data() != nullptr);  // an http or https URL has an authority
  const auto originSize = static_cast<std::size_t>(parts.host.data() + parts.host.size() - url.data());
  assert(!url.substr(0, originSize).contains('@'));

  _chars.reserve(url.size() + 1U);  // the only step that can throw: the URL stays valid
  char* pData = _chars.data();

  pData = Append(url, pData);
  *pData++ = '\0';

  _chars.setEnd(pData);

  _originSize = static_cast<uint32_t>(originSize);
}

}  // namespace aeronet::internal
