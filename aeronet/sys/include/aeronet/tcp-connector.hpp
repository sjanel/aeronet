#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/connection.hpp"

struct addrinfo;

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
// - port: numeric TCP port to connect to
// - family: address family hint (0 == unspecified == AF_UNSPEC)
// - connectTimeoutMs: > 0 enables blocking multi-address fallback within this millisecond budget
// Note: the host buffer pointed by the given span SHOULD be 1 byte writable at its end,
// as getaddrinfo expects a null-terminated string.
ConnectResult ConnectTCP(std::span<char> host, uint16_t port, int family = 0, int connectTimeoutMs = 0);

struct AddrInfoDeleter {
  void operator()(addrinfo* addresses) const noexcept;
};

// Addresses returned by getaddrinfo, freed with freeaddrinfo.
using AddrInfoPtr = std::unique_ptr<addrinfo, AddrInfoDeleter>;

// Resolve host:port to its TCP addresses with getaddrinfo, which blocks until the resolver answers.
// Returns nullptr on failure, after logging it. Same requirement on 'host' as ConnectTCP().
[[nodiscard]] AddrInfoPtr ResolveTCP(std::span<char> host, uint16_t port, int family = 0);

// Connect to one of the given resolved addresses, with the same address fallback and connectTimeoutMs semantics as
// ConnectTCP(host, port, ...).
[[nodiscard]] ConnectResult ConnectTCP(const addrinfo& addresses, int connectTimeoutMs = 0);

// Tell whether 'host' is a numeric IPv4 or IPv6 address (without brackets), that getaddrinfo resolves without query.
[[nodiscard]] bool IsNumericHost(std::string_view host) noexcept;

}  // namespace aeronet
