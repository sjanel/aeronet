#pragma once

#include <cstdint>

namespace aeronet {

enum class TcpNoDelayMode : std::uint8_t {
  // Auto-detect based on configuration: enabled for TLS and HTTP/2, disabled otherwise, unless explicitly overridden by
  // user. Each SSL_write produces small TLS records that would otherwise stall on the Nagle + Delayed-ACK interaction.
  Auto,

  // Always disable TCP_NODELAY (enable Nagle).
  Disabled,

  // Always enable TCP_NODELAY (disable Nagle).
  Enabled,
};

}