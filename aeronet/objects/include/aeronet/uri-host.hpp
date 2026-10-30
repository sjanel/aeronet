#pragma once

#include <string_view>

namespace aeronet::http {

/// Tell whether 'addr' is an IPv4address (RFC 3986 §3.2.2): 4 numbers in [0, 255] without leading zero, separated by
/// '.'.
bool IsValidIPv4Address(std::string_view addr) noexcept;

/// Tell whether 'addr' is an IPv6address (RFC 3986 §3.2.2), without brackets: 8 pieces of 1 to 4 hexadecimal digits
/// separated by ':', of which a single "::" stands for one or more zero pieces, the last 2 pieces possibly written as
/// an IPv4address.
bool IsValidIPv6Address(std::string_view addr) noexcept;

/// Tell whether 'host' is a non-empty reg-name (RFC 3986 §3.2.2), which includes IPv4 addresses.
bool IsValidRegName(std::string_view host) noexcept;

/// Tell whether 'authority' is uri-host [ ":" port ] (RFC 9110 §4.2.1, §7.2), with a non-empty host: the syntax of the
/// Host header field, of the HTTP/2 :authority pseudo-header field and of an http(s) URI authority without userinfo.
/// The host is a reg-name (or an IPv4 address), or an IPv6address or IPvFuture enclosed in brackets (RFC 3986 §3.2.2),
/// the port is made of digits only, possibly none.
bool IsValidHostAndPort(std::string_view authority) noexcept;

/// Tell whether 'value' is a valid Host header field value (RFC 9110 §7.2): uri-host [ ":" port ], or empty (sent for
/// a target URI without authority).
inline bool IsValidHostFieldValue(std::string_view value) noexcept {
  return value.empty() || IsValidHostAndPort(value);
}

}  // namespace aeronet::http
