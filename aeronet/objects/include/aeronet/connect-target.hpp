#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

#include "aeronet/uri-host.hpp"

namespace aeronet {

/// Target of a CONNECT request, parsed from the authority form "host:port" (RFC 9110 §9.3.6).
struct ConnectTarget {
  /// Parse 'authority'. An IPv6 address is enclosed in brackets ("[::1]:443"), which are not part of host.
  /// The target is invalid() when the host is not a reg-name nor an IPv6 address (RFC 3986 §3.2.2), or the port is
  /// missing or not a number in [1, 65535] (RFC 3986 port = *DIGIT: a service name such as "https" is invalid).
  explicit ConnectTarget(std::string_view authority) noexcept {
    std::string_view portPart;
    bool validHost;
    if (authority.starts_with('[')) {
      const auto closePos = authority.find(']');
      if (closePos == std::string_view::npos || closePos + 1 == authority.size() || authority[closePos + 1] != ':') {
        return;
      }
      host = authority.substr(1, closePos - 1);
      validHost = http::IsValidIPv6Address(host);
      portPart = authority.substr(closePos + 2);
    } else {
      // A host name or an IPv4 address contains no colon.
      const auto colonPos = authority.find(':');
      if (colonPos == std::string_view::npos) {
        return;
      }
      host = authority.substr(0, colonPos);
      validHost = http::IsValidRegName(host);
      portPart = authority.substr(colonPos + 1);
    }
    const auto [ptr, errc] = std::from_chars(portPart.data(), portPart.data() + portPart.size(), port);
    if (!validHost || errc != std::errc{} || ptr != portPart.data() + portPart.size()) {
      port = 0;
    }
  }

  /// Tell whether the authority could not be parsed.
  [[nodiscard]] bool invalid() const noexcept { return port == 0; }

  std::string_view host;
  uint16_t port{0};
};

}  // namespace aeronet
