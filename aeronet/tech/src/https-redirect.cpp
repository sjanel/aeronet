#include "aeronet/https-redirect.hpp"

#include <charconv>
#include <cstdint>
#include <string_view>

#include "aeronet/memory-utils.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/url-encode.hpp"

namespace aeronet::http {

namespace {
constexpr std::string_view kHttpsScheme = "https://";
constexpr uint16_t kStandardHttpsPort = 443;

constexpr bool IsUnreserved(char ch) noexcept {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' ||
         ch == '_' || ch == '~';
}

// Characters allowed verbatim in a URL path: unreserved + path sub-delims + ":" / "@" / "/".
// '%' is intentionally excluded so a decoded literal '%' is re-encoded to "%25".
constexpr bool IsPathSafe(char ch) noexcept {
  switch (ch) {
    case '/':
    case ':':
    case '@':
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=':
      return true;
    default:
      return IsUnreserved(ch);
  }
}

}  // namespace

std::string_view StripPortFromHost(std::string_view hostHeader) noexcept {
  if (hostHeader.empty()) {
    return hostHeader;
  }
  if (hostHeader.front() == '[') {
    // Bracketed IPv6 literal: keep everything up to and including the closing ']'.
    const auto closing = hostHeader.find(']');
    if (closing != std::string_view::npos) {
      return hostHeader.substr(0, closing + 1);
    }
    return hostHeader;  // malformed, leave as-is
  }
  // Plain hostname or IPv4: a single ':' separates the optional port.
  const auto colon = hostHeader.find(':');
  if (colon != std::string_view::npos) {
    return hostHeader.substr(0, colon);
  }
  return hostHeader;
}

bool AppendHttpsAuthority(RawChars& out, std::string_view hostHeader, uint16_t targetPort) {
  out.clear();

  const std::string_view host = StripPortFromHost(hostHeader);
  if (host.empty()) {
    return false;
  }

  const bool addPort = targetPort != kStandardHttpsPort;
  const auto hostNbDigits = addPort ? ndigits(targetPort) : 0;

  std::size_t requiredCapacity = kHttpsScheme.size() + host.size();
  if (addPort) {
    requiredCapacity += 1 + static_cast<std::size_t>(hostNbDigits);  // ':' + port digits
  }

  out.reserve(requiredCapacity);
  char* pEnd = out.data();

  pEnd = Append(kHttpsScheme, pEnd);
  pEnd = Append(host, pEnd);

  if (addPort) {
    *pEnd++ = ':';
    pEnd = std::to_chars(pEnd, out.data() + out.capacity(), targetPort).ptr;
  }
  out.setSize(static_cast<RawChars::size_type>(pEnd - out.data()));
  return true;
}

void AppendUrlEncodedPath(RawChars& out, std::string_view decodedPath) {
  out.ensureAvailableCapacity(URLEncodedSize(decodedPath, IsPathSafe));
  char* pEnd = URLEncode(decodedPath, IsPathSafe, out.data() + out.size());
  out.setSize(static_cast<RawChars::size_type>(pEnd - out.data()));
}

void AppendUrlEncodedQueryParam(RawChars& out, char leadingChar, std::string_view decodedKey,
                                std::string_view decodedValue) {
  out.ensureAvailableCapacity(1UL + URLEncodedSize(decodedKey, IsUnreserved) + 1UL +
                              URLEncodedSize(decodedValue, IsUnreserved));

  char* pEnd = out.data() + out.size();

  *pEnd++ = leadingChar;
  pEnd = URLEncode(decodedKey, IsUnreserved, pEnd);
  *pEnd++ = '=';
  pEnd = URLEncode(decodedValue, IsUnreserved, pEnd);
  out.setSize(static_cast<RawChars::size_type>(pEnd - out.data()));
}

}  // namespace aeronet::http
