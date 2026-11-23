#pragma once

#include <cstdint>
#include <string_view>

struct sockaddr;

namespace aeronet {

// The IP address spaces of the Local Network Access specification of browsers (WICG), from the most private to the
// most public. A web page reaches only the servers of its own address space or of a more public one: a page of the
// Internet cannot make the client send requests to the host itself nor to its local network (see
// PageRequestContext::addressSpace).
enum class AddressSpace : std::uint8_t {
  // The host itself: 127.0.0.0/8, ::1, the unspecified addresses 0.0.0.0 and :: (a connection to them reaches the
  // host), and 198.18.0.0/15.
  Loopback,

  // A local network: the private-use (10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16), carrier-grade NAT (100.64.0.0/10),
  // link-local (169.254.0.0/16, fe80::/10), unique-local (fc00::/7) and site-local (fec0::/10) addresses, the rest of
  // 0.0.0.0/8, and the IPv6 documentation prefixes (2001:db8::/32, 3fff::/20).
  Local,

  // Every other address.
  Public,
};

// The address space of 'addr', an AF_INET or AF_INET6 socket address, after the table of the Local Network Access
// specification. An IPv6 address embedding an IPv4 one (IPv4-mapped ::ffff:0:0/96, and NAT64 64:ff9b::/96, which the
// specification does not list) is classified as that IPv4 address. Loopback for the other families.
[[nodiscard]] AddressSpace AddressSpaceOf(const sockaddr& addr) noexcept;

// The address space of the host of a URL, without resolving it: that of an IPv4 or IPv6 address (without brackets),
// the IPv4 ones in any form that browsers and inet_aton() read ("127.1", "2130706433", "0x7f.0.0.1"), Loopback for
// "localhost" and the names under it (RFC 6761 §6.3) and for the invalid addresses (a host with a colon, or ending in a
// number, that is not one: a resolver might still read it as an address), Public for the other names, whose addresses
// are unknown.
[[nodiscard]] AddressSpace HostAddressSpace(std::string_view host) noexcept;

}  // namespace aeronet
