#include "aeronet/uri-host.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string_view>

#include "aeronet/cctype.hpp"
#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/compiler-config.hpp"

namespace aeronet::http {

namespace {

constexpr bool IsHexDigit(char ch) noexcept { return FromHexDigit(ch) >= 0; }

// RFC 3986 §3.2.2: the characters of a reg-name, besides its pct-encoded octets: unreserved / sub-delims.
constexpr bool IsRegNameChar(char ch) noexcept {
  static constexpr auto kRegNameCharTable = [] {
    struct {
      bool data[256];
    } table{};
    constexpr std::string_view kSymbols = "-._~!$&'()*+,;=";
    for (int ic = 0; ic < 256; ++ic) {
      const char ch = static_cast<char>(ic);
      table.data[ic] = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || isdigit(ch) || kSymbols.contains(ch);
    }
    return table;
  }();
  return kRegNameCharTable.data[static_cast<unsigned char>(ch)];
}

// Length of the longest prefix of 'str' that is a reg-name (RFC 3986 §3.2.2):
// reg-name = *( unreserved / pct-encoded / sub-delims ), where pct-encoded = "%" HEXDIG HEXDIG.
constexpr std::size_t RegNameLength(std::string_view str) noexcept {
  // The scan of the characters other than pct-encoded octets is kept free of their handling, so that it stays a tight
  // loop. A captureless lambda (not a function): find_if_not monomorphizes on its closure type, inlining the predicate.
  constexpr auto isRegNameChar = [](char ch) { return IsRegNameChar(ch); };
  auto it = str.begin();
  while (true) {
    it = std::find_if_not(it, str.end(), isRegNameChar);
    if (str.end() - it < 3 || *it != '%' || !IsHexDigit(it[1]) || !IsHexDigit(it[2])) {
      return static_cast<std::size_t>(it - str.begin());
    }
    it += 3;
  }
}

// The address checks are implemented here, implicitly inline (constexpr), rather than in the exported functions: the
// compiler then inlines IsIPv6Address in IPLiteralLength, as it does not for a non-inline function of the same size.
constexpr bool IsIPv4Address(std::string_view addr) noexcept {
  std::size_t pos = 0;
  for (int octetIdx = 0; octetIdx < 4; ++octetIdx) {
    if (octetIdx != 0) {
      if (pos == addr.size() || addr[pos] != '.') {
        return false;
      }
      ++pos;
    }
    const std::size_t octetBeg = pos;
    unsigned int value = 0;
    while (pos < addr.size() && pos - octetBeg < 3U && isdigit(addr[pos])) {
      value = (value * 10U) + static_cast<unsigned int>(addr[pos] - '0');
      ++pos;
    }
    const std::size_t octetLen = pos - octetBeg;
    if (octetLen == 0 || value > 255U || (octetLen > 1U && addr[octetBeg] == '0')) {
      return false;
    }
  }
  return pos == addr.size();
}

constexpr bool IsIPv6Address(std::string_view addr) noexcept {
  std::size_t nbPieces = 0;
  bool elided = addr.starts_with("::");
  std::size_t pos = elided ? 2U : 0U;
  while (pos < addr.size()) {
    const std::size_t pieceBeg = pos;
    while (pos < addr.size() && IsHexDigit(addr[pos])) {
      ++pos;
    }
    if (pos < addr.size() && addr[pos] == '.') {
      // The digits were the first number of an IPv4address, which ends the address.
      if (!IsIPv4Address(addr.substr(pieceBeg))) {
        return false;
      }
      nbPieces += 2U;
      break;
    }
    if (pos == pieceBeg || pos - pieceBeg > 4U) {
      return false;
    }
    ++nbPieces;
    if (pos == addr.size()) {
      break;
    }
    if (addr[pos] != ':') {
      return false;
    }
    ++pos;
    if (pos == addr.size()) {
      return false;  // a single trailing ':'
    }
    if (addr[pos] == ':') {
      if (elided) {
        return false;  // a second "::"
      }
      elided = true;
      ++pos;
    }
  }
  return elided ? nbPieces < 8U : nbPieces == 8U;
}

// Length of the IP-literal (RFC 3986 §3.2.2) that starts 'str', which starts with '[', or 0 if there is none:
// IP-literal = "[" ( IPv6address / IPvFuture ) "]", IPvFuture = "v" 1*HEXDIG "." 1*( unreserved / sub-delims / ":" ).
// Out of line: rare, it keeps the code of the common case (a host name) small, without registers to save.
AERONET_NOINLINE std::size_t IPLiteralLength(std::string_view str) noexcept {
  assert(str.starts_with('['));
  const std::size_t closePos = str.find(']');
  if (closePos == std::string_view::npos) {
    return 0;
  }
  const std::string_view literal = str.substr(1, closePos - 1);
  if (literal.starts_with('v') || literal.starts_with('V')) {
    const std::size_t dotPos = literal.find('.');
    if (dotPos == std::string_view::npos || dotPos == 1U || dotPos + 1U == literal.size() ||
        !std::ranges::all_of(literal.substr(1, dotPos - 1), IsHexDigit) ||
        !std::ranges::all_of(literal.substr(dotPos + 1), [](char ch) { return IsRegNameChar(ch) || ch == ':'; })) {
      return 0;
    }
  } else if (!IsIPv6Address(literal)) {
    return 0;
  }
  return closePos + 1U;
}

}  // namespace

bool IsValidIPv4Address(std::string_view addr) noexcept { return IsIPv4Address(addr); }

bool IsValidIPv6Address(std::string_view addr) noexcept { return IsIPv6Address(addr); }

bool IsValidRegName(std::string_view host) noexcept { return !host.empty() && RegNameLength(host) == host.size(); }

bool IsValidHostAndPort(std::string_view authority) noexcept {
  const std::size_t hostEnd = authority.starts_with('[') ? IPLiteralLength(authority) : RegNameLength(authority);
  if (hostEnd == 0) {
    return false;
  }
  return hostEnd == authority.size() ||
         (authority[hostEnd] == ':' &&
          std::ranges::all_of(authority.substr(hostEnd + 1), [](char ch) { return isdigit(ch); }));
}

}  // namespace aeronet::http
