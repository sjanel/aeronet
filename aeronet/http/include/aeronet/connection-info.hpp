#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/socket-ops.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-info.hpp"
#endif

namespace aeronet {

// What the requests of a connection know about it: its TLS session, its peer and the number of requests it served.
// The server keeps it in the state of the connection (ConnectionState derives from it).
struct ConnectionInfo {
  // Best-effort client address string captured from the peer socket address (empty if it is not an IP address).
  [[nodiscard]] std::string_view clientAddress() const noexcept { return {clientAddressBuffer, clientAddressLength}; }

#ifdef AERONET_ENABLE_OPENSSL
  TLSInfo tlsInfo;
#endif
  uint32_t requestsServed{0};
  char clientAddressBuffer[kFormattedAddressCapacity];
  uint8_t clientAddressLength{0};
};

}  // namespace aeronet
