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
// indicating whether the connect is still pending (non-blocking error::kInProgress).
//
// Address fallback: getaddrinfo may return several candidates (e.g. "localhost" -> ::1 and 127.0.0.1).
// A synchronous connect() failure (ECONNREFUSED, ...) is transparently retried on the next candidate.
// A non-blocking connect() however returns error::kInProgress immediately, which *defers* the real
// outcome and would otherwise hide a failed first candidate. Pass connectTimeoutMs > 0 to opt into a
// blocking fallback: ConnectTCP then waits (up to the given budget, shared across candidates) for each
// pending connect to resolve and, on failure, falls back to the next candidate. The returned socket is
// then already connected (connectPending == false). With connectTimeoutMs == 0 (the default) the first
// pending connect is returned as-is (connectPending == true) for the caller to complete via its own
// event loop — used by the server where blocking the loop is not acceptable.
//
// Note: the default family value is 0 (unspecified). We avoid using
// platform macros like AF_UNSPEC in the header to keep includes minimal.
// Parameters:
// - host: span hostname or IP address to connect to
// - port: span port number or service name
// - family: address family hint (0 == unspecified == AF_UNSPEC)
// - connectTimeoutMs: > 0 enables blocking multi-address fallback within this millisecond budget
// Note: the buffers pointed by given spans SHOULD be 1 byte writable at their end,
// as getaddrinfo expects null-terminated strings.
ConnectResult ConnectTCP(std::span<char> host, std::span<char> port, int family = 0, int connectTimeoutMs = 0);

}  // namespace aeronet
