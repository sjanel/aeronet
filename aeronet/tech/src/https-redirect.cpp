#include "aeronet/https-redirect.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/nchars.hpp"
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
// Backed by a compile-time 256-entry lookup table so the per-char test (run twice over the path:
// once to size, once to encode) is a single load instead of a branch chain.
constexpr bool IsPathSafe(char ch) noexcept {
  static constexpr auto kPathSafeTable = [] {
    struct {
      bool data[256];
    } table{};
    constexpr std::string_view kPathExtra = "/:@!$&'()*+,;=";
    for (int ic = 0; ic < 256; ++ic) {
      const char cc = static_cast<char>(ic);
      table.data[ic] = IsUnreserved(cc) || kPathExtra.contains(cc);
    }
    return table;
  }();
  return kPathSafeTable.data[static_cast<unsigned char>(ch)];
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
  const std::uint8_t hostNbChars = addPort ? nchars(targetPort) : 0;

  std::size_t requiredCapacity = kHttpsScheme.size() + host.size();
  if (addPort) {
    requiredCapacity += 1U + hostNbChars;  // ':' + port digits
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
  // Captureless lambda (not the function name): the template monomorphizes on its unique closure type, so the
  // per-char predicate inlines into the encode loop instead of becoming an indirect call through a function pointer.
  constexpr auto isPathSafe = [](char ch) { return IsPathSafe(ch); };

  out.ensureAvailableCapacity(URLEncodedSize(decodedPath, isPathSafe));
  char* pEnd = URLEncode(decodedPath, isPathSafe, out.data() + out.size());

  out.setSize(static_cast<RawChars::size_type>(pEnd - out.data()));
}

void AppendUrlEncodedQueryParam(RawChars& out, char leadingChar, std::string_view decodedKey,
                                std::string_view decodedValue) {
  constexpr auto isUnreserved = [](char ch) { return IsUnreserved(ch); };

  out.ensureAvailableCapacity(1UL + URLEncodedSize(decodedKey, isUnreserved) + 1UL +
                              URLEncodedSize(decodedValue, isUnreserved));

  char* pEnd = out.data() + out.size();

  *pEnd++ = leadingChar;
  pEnd = URLEncode(decodedKey, isUnreserved, pEnd);
  *pEnd++ = '=';
  pEnd = URLEncode(decodedValue, isUnreserved, pEnd);

  out.setSize(static_cast<RawChars::size_type>(pEnd - out.data()));
}

}  // namespace aeronet::http
