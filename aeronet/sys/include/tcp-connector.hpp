#pragma once

#include <string_view>

#include "connection.hpp"

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
ConnectResult ConnectTCP(char *buf, std::string_view host, std::string_view port, int family = 0);

}  // namespace aeronet
