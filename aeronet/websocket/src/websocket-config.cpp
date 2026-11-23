#include "aeronet/websocket-config.hpp"

#include <stdexcept>

namespace aeronet::websocket {

void WebSocketConfig::validate() const {
  if (maxMessageSize == 0) {
    throw std::invalid_argument("WebSocketConfig: maxMessageSize must be greater than 0");
  }
  if (maxFrameSize == 0) {
    throw std::invalid_argument("WebSocketConfig: maxFrameSize must be greater than 0");
  }
  // 0 closes the connection as soon as its Close frame is sent, max() waits for the answer of the peer forever.
  if (closeTimeout.count() < 0) {
    throw std::invalid_argument("WebSocketConfig: closeTimeout must not be negative");
  }
  // max() disables it.
  if (idleTimeout.count() <= 0) {
    throw std::invalid_argument("WebSocketConfig: idleTimeout must be greater than 0");
  }
#ifdef AERONET_ENABLE_ZLIB
  deflateConfig.validate();
#endif
}

}  // namespace aeronet::websocket
