#pragma once

#include <chrono>
#include <cstddef>

#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-deflate.hpp"

namespace aeronet::websocket {

/// Configuration options for WebSocket connections.
struct WebSocketConfig {
  /// Checks the configuration, throws std::invalid_argument if invalid (a null size limit, or an invalid
  /// deflateConfig).
  void validate() const;

  /// Maximum size of a single message (after reassembly from fragments, and decompression).
  /// Must be greater than 0. To approximate no limit, set std::numeric_limits<std::size_t>::max() (use with caution).
  std::size_t maxMessageSize{kDefaultMaxMessageSize};

  /// Maximum size of a single frame payload.
  /// Must be greater than 0. To approximate no limit, set std::numeric_limits<std::size_t>::max() (use with caution).
  std::size_t maxFrameSize{kDefaultMaxFrameSize};

  /// Time to wait for the peer's Close frame after sending one. The connection is then closed.
  std::chrono::milliseconds closeTimeout{std::chrono::milliseconds{5000}};

  /// A connection on which nothing was received for this long is closed. 0 disables the idle timeout.
  /// It replaces HttpServerConfig::keepAliveTimeout, which does not apply to upgraded connections.
  std::chrono::milliseconds idleTimeout{std::chrono::seconds{60}};

  /// Deflate configuration (optional, for permessage-deflate extension).
  DeflateConfig deflateConfig;

  /// Whether this is the server side (affects masking validation).
  bool isServerSide{true};

  /// Send a Ping frame once the connection has been idle for half of idleTimeout, so that a live peer, which answers
  /// with a Pong, never reaches the idle timeout. Without effect when idleTimeout is 0.
  bool autoPing{true};
};

}  // namespace aeronet::websocket
