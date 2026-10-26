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
  deflateConfig.validate();
}

}  // namespace aeronet::websocket
