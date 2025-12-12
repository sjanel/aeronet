#pragma once

#include <span>

#include "aeronet/connection.hpp"

namespace aeronet {

struct ConnectResult {
  Connection cnx;
  bool connectPending{false};
  bool failure{false};
};

// Attempt to resolve host:port and connect to one of the returned addresses.
// On success returns a ConnectResult owning the connected socket and a flag
// indicating whether the connect is still pending (non-blocking EINPROGRESS).
// On failure returns std::nullopt.
//
// Note: the default family value is 0 (unspecified). We avoid using
// platform macros like AF_UNSPEC in the header to keep includes minimal.
// Parameters:
// - host: span hostname or IP address to connect to
// - port: span port number or service name
// Note: the buffers pointed by given spans SHOULD be 1 byte writable at their end,
// as getaddrinfo expects null-terminated strings.
ConnectResult ConnectTCP(std::span<char> host, std::span<char> port, int family = 0);

}  // namespace aeronet
