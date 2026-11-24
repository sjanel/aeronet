#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/http-request.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::internal {

// Largest response of a proxy to a CONNECT request: a misbehaving proxy cannot make the client read unbounded data.
inline constexpr std::size_t kMaxProxyTunnelResponse = 8192;

// Write in 'out' (cleared first) the CONNECT request opening a tunnel to the origin of 'req' through a forward proxy,
// with the explicit "host:port" authority it needs (RFC 9110 §9.3.6), IPv6 literals bracketed.
void BuildProxyConnectRequest(const HttpRequest& req, RawChars& out);

// What the bytes received from a proxy after a CONNECT request tell about the tunnel.
enum class ProxyTunnelStatus : uint8_t {
  Incomplete,  // the head of the response is not complete yet: read more
  Open,        // a 2xx response: the tunnel is open
  Refused,     // another response, bytes that are not a response, or a head larger than kMaxProxyTunnelResponse
};

// The status of the tunnel after 'received', the bytes received from the proxy so far. Nothing beyond the head of the
// response is expected: the server says nothing before the client speaks through the tunnel.
[[nodiscard]] ProxyTunnelStatus CheckProxyTunnelResponse(std::string_view received) noexcept;

}  // namespace aeronet::internal
