#include "aeronet/http-message-data.hpp"

#include <cstddef>
#include <string_view>
#include <utility>

#include "aeronet/embedded-payload.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

HttpMessageData::HttpMessageData(RawChars head, HttpPayload body) : _buf(std::move(head)), _headSize(_buf.size()) {
  if (!body.empty()) {
    EmbeddedPayload::Construct(_buf, _headSize, std::move(body));
  }
}

char* HttpMessageData::resizeUp(std::size_t sz) {
  if (hasPayload()) {
    HttpPayload* pBody = payload();
    const std::size_t oldSize = pBody->size();
    pBody->ensureAvailableCapacity(sz);
    pBody->addSize(sz);
    return pBody->data() + oldSize;
  }
  _buf.ensureAvailableCapacity(sz);
  char* pData = _buf.end();
  _buf.addSize(sz);
  _headSize = _buf.size();
  return pData;
}

void HttpMessageData::append(std::string_view sv) {
  if (hasPayload()) {
    // The head buffer cannot grow while it hosts the payload object, which is anyway at the end of the message.
    payload()->append(sv);
  } else {
    _buf.append(sv);
    _headSize = _buf.size();
  }
}

void HttpMessageData::clear() noexcept {
  if (hasPayload()) {
    destroyPayload();
  }
  _buf.clear();
  _headSize = 0;
  _offset = 0;
}

void HttpMessageData::shrink_to_fit() {
  if (hasPayload()) {
    // The head buffer cannot be reallocated while it hosts the payload object.
    payload()->shrink_to_fit();
  } else {
    _buf.shrink_to_fit();
  }
}

void HttpMessageData::destroyPayload() noexcept { EmbeddedPayload::Destroy(_buf, _headSize); }

}  // namespace aeronet
