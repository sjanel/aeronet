#include "aeronet/address-space.hpp"

#ifdef AERONET_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "aeronet/cctype.hpp"
#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet {

namespace {

// A block of IPv4 addresses of the table of the specification.
struct Ipv4Block {
  uint32_t prefix;
  uint32_t mask;
  AddressSpace space;
};

// The non-public IPv4 blocks, in the order of the table of the specification: the first match wins (0.0.0.0/32 before
// 0.0.0.0/8).
constexpr Ipv4Block kIpv4Blocks[]{
    {0x7F000000U, 0xFF000000U, AddressSpace::Loopback},  // 127.0.0.0/8
    {0x0A000000U, 0xFF000000U, AddressSpace::Local},     // 10.0.0.0/8
    {0x64400000U, 0xFFC00000U, AddressSpace::Local},     // 100.64.0.0/10
    {0xAC100000U, 0xFFF00000U, AddressSpace::Local},     // 172.16.0.0/12
    {0xC0A80000U, 0xFFFF0000U, AddressSpace::Local},     // 192.168.0.0/16
    {0xC6120000U, 0xFFFE0000U, AddressSpace::Loopback},  // 198.18.0.0/15
    {0xA9FE0000U, 0xFFFF0000U, AddressSpace::Local},     // 169.254.0.0/16
    {0x00000000U, 0xFFFFFFFFU, AddressSpace::Loopback},  // 0.0.0.0/32
    {0x00000000U, 0xFF000000U, AddressSpace::Local},     // 0.0.0.0/8
};

// The 4 bytes at 'bytes' as an IPv4 address in host order (network order in memory).
constexpr uint32_t Ipv4At(const unsigned char* bytes) noexcept {
  return (uint32_t{bytes[0]} << 24U) | (uint32_t{bytes[1]} << 16U) | (uint32_t{bytes[2]} << 8U) | uint32_t{bytes[3]};
}

constexpr AddressSpace Ipv4AddressSpace(uint32_t addr) noexcept {
  for (const Ipv4Block& block : kIpv4Blocks) {
    if ((addr & block.mask) == block.prefix) {
      return block.space;
    }
  }
  return AddressSpace::Public;
}

// The address space of the IPv6 address of the 16 bytes at 'bytes' (network order).
AddressSpace Ipv6AddressSpace(const unsigned char* bytes) noexcept {
  static constexpr std::size_t kPrefixSize = 12;
  // ::ffff:0:0/96 (IPv4-mapped) and 64:ff9b::/96 (NAT64, RFC 6052): the address of their last 4 bytes.
  static constexpr unsigned char kIpv4Mapped[kPrefixSize]{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
  static constexpr unsigned char kNat64[kPrefixSize]{0, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0};
  if (std::memcmp(bytes, kIpv4Mapped, kPrefixSize) == 0 || std::memcmp(bytes, kNat64, kPrefixSize) == 0) {
    return Ipv4AddressSpace(Ipv4At(bytes + kPrefixSize));
  }
  // ::1/128 (loopback) and ::/128 (unspecified)
  static constexpr unsigned char kZeros[15]{};
  if (std::memcmp(bytes, kZeros, sizeof(kZeros)) == 0 && bytes[15] <= 1U) {
    return AddressSpace::Loopback;
  }
  const bool uniqueLocal = (bytes[0] & 0xFEU) == 0xFCU;                       // fc00::/7
  const bool linkOrSiteLocal = bytes[0] == 0xFEU && (bytes[1] & 0x80U) != 0;  // fe80::/10, fec0::/10
  const bool documentation =
      (bytes[0] == 0x20U && bytes[1] == 0x01U && bytes[2] == 0x0DU && bytes[3] == 0xB8U) ||  // 2001:db8::/32
      (bytes[0] == 0x3FU && bytes[1] == 0xFFU && (bytes[2] & 0xF0U) == 0);                   // 3fff::/20
  return uniqueLocal || linkOrSiteLocal || documentation ? AddressSpace::Local : AddressSpace::Public;
}

// The value of Ipv4Number() for a part that is not a number.
constexpr uint64_t kNotANumber = ~uint64_t{};

// The value of 'part' of an IPv4 address in the forms of the WHATWG URL Standard ("IPv4 number parser", as
// inet_aton()): decimal, octal with a leading 0, hexadecimal with a leading 0x ("0x" alone is 0). Saturated at 2^32,
// which no part of an address reaches.
constexpr uint64_t Ipv4Number(std::string_view part) noexcept {
  if (part.empty()) {
    return kNotANumber;
  }
  uint64_t radix = 10;
  if (part.size() > 1 && part[0] == '0') {
    const bool hexadecimal = part[1] == 'x' || part[1] == 'X';
    radix = hexadecimal ? 16 : 8;
    part.remove_prefix(hexadecimal ? 2U : 1U);
  }
  static constexpr uint64_t kSaturated = uint64_t{1} << 32U;
  uint64_t value = 0;
  for (const char ch : part) {
    // -1 (not a hexadecimal digit) becomes 255, beyond every radix.
    const auto digit = static_cast<uint8_t>(FromHexDigit(ch));
    if (digit >= radix) {
      return kNotANumber;
    }
    value = std::min((value * radix) + digit, kSaturated);
  }
  return value;
}

// Whether 'host', without its final dot, ends in a number (WHATWG URL "ends in a number checker"): its last label is
// made of decimal digits, or is a number of Ipv4Number(). Browsers read such a host as an IPv4 address, or reject it.
constexpr bool EndsInANumber(std::string_view host) noexcept {
  const std::string_view last = host.substr(host.rfind('.') + 1);
  return !last.empty() && (std::ranges::all_of(last, isdigit) || Ipv4Number(last) != kNotANumber);
}

// Parses 'host', without its final dot, as an IPv4 address in the forms of the WHATWG URL Standard ("IPv4 parser", as
// inet_aton()) into 'address' (host order): 1 to 4 numbers of Ipv4Number() separated by dots, the last one filling the
// remaining bytes ("127.1" and "2130706433" are 127.0.0.1). Returns false if 'host' is not such an address.
constexpr bool ParseIpv4(std::string_view host, uint32_t& address) noexcept {
  uint64_t value = 0;
  for (uint32_t shift = 24;; shift -= 8U) {
    const std::size_t dot = host.find('.');
    const uint64_t number = Ipv4Number(host.substr(0, dot));
    if (dot == std::string_view::npos) {
      if (number >= (uint64_t{1} << (shift + 8U))) {
        return false;
      }
      address = static_cast<uint32_t>(value | number);
      return true;
    }
    // A part before the last one is a byte, and an address has 4 parts at most.
    if (number > 0xFFU || shift == 0) {
      return false;
    }
    value |= number << shift;
    host.remove_prefix(dot + 1);
  }
}

}  // namespace

AddressSpace AddressSpaceOf(const sockaddr& addr) noexcept {
  if (addr.sa_family == AF_INET) {
    const auto& ipv4 = reinterpret_cast<const sockaddr_in&>(addr);
    unsigned char bytes[4];
    std::memcpy(bytes, &ipv4.sin_addr, sizeof(bytes));
    return Ipv4AddressSpace(Ipv4At(bytes));
  }
  if (addr.sa_family == AF_INET6) {
    return Ipv6AddressSpace(reinterpret_cast<const sockaddr_in6&>(addr).sin6_addr.s6_addr);
  }
  return AddressSpace::Loopback;
}

AddressSpace HostAddressSpace(std::string_view host) noexcept {
  // A host with a colon is an IPv6 address (a name has none), with or without a zone identifier ("fe80::1%eth0").
  if (host.contains(':')) {
    sockaddr_storage addr;
    return ParseAddress(host.substr(0, host.find('%')), addr) ? AddressSpaceOf(reinterpret_cast<const sockaddr&>(addr))
                                                              : AddressSpace::Loopback;
  }
  // The final dot of a fully qualified name.
  if (host.ends_with('.')) {
    host.remove_suffix(1);
  }
  // A host ending in a number is an IPv4 address, in any of the forms that browsers read, as inet_aton() and
  // getaddrinfo() do ("127.1", "0x7f.0.0.1"): a proxy would connect to it. When it is not a valid one (browsers reject
  // it), a proxy could still read it as an address: it is taken as the host itself, the most private, as an invalid
  // IPv6 address.
  if (EndsInANumber(host)) {
    uint32_t ipv4;
    return ParseIpv4(host, ipv4) ? Ipv4AddressSpace(ipv4) : AddressSpace::Loopback;
  }
  // "localhost" and the names under it.
  static constexpr std::string_view kLocalhost = "localhost";
  if (host.size() >= kLocalhost.size() &&
      CaseInsensitiveEqual(host.substr(host.size() - kLocalhost.size()), kLocalhost) &&
      (host.size() == kLocalhost.size() || host[host.size() - kLocalhost.size() - 1U] == '.')) {
    return AddressSpace::Loopback;
  }
  return AddressSpace::Public;
}

}  // namespace aeronet
