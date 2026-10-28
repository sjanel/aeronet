#include "aeronet/https-redirect.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/internal/is-path-safe.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/url-encode.hpp"

namespace aeronet::http {

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

bool SetHttpsAuthority(RawChars& out, std::string_view hostHeader, uint16_t targetPort) {
  out.clear();

  const std::string_view host = StripPortFromHost(hostHeader);
  if (host.empty()) {
    return false;
  }

  static constexpr decltype(targetPort) kStandardHttpsPort = 443;

  const bool addPort = targetPort != kStandardHttpsPort;
  const std::uint8_t hostNbDigits = addPort ? ndigits(targetPort) : 0;

  static constexpr std::string_view kHttpsScheme = "https://";

  std::size_t requiredCapacity = kHttpsScheme.size() + host.size();
  if (addPort) {
    requiredCapacity += 1U + hostNbDigits;  // ':' + port digits
  }

  out.reserve(requiredCapacity);
  char* pEnd = out.data();

  pEnd = AppendFixed<kHttpsScheme>(pEnd);
  pEnd = Append(host, pEnd);

  if (addPort) {
    *pEnd++ = ':';
    pEnd = WriteUInt(pEnd, targetPort, hostNbDigits);
  }
  out.setEnd(pEnd);
  return true;
}

void AppendUrlEncodedPath(RawChars& out, std::string_view decodedPath) {
  // Captureless lambda (not the function name): the template monomorphizes on its unique closure type, so the
  // per-char predicate inlines into the encode loop instead of becoming an indirect call through a function pointer.
  constexpr auto isPathSafe = [](char ch) { return internal::IsPathSafe(ch); };

  out.ensureAvailableCapacityExponential(URLEncodedSize(decodedPath, isPathSafe));
  char* pEnd = URLEncode(decodedPath, isPathSafe, out.data() + out.size());

  out.setEnd(pEnd);
}

void AppendUrlEncodedQueryParam(RawChars& out, char leadingChar, std::string_view decodedKey,
                                std::string_view decodedValue) {
  constexpr auto isUnreserved = [](char ch) { return internal::IsUnreserved(ch); };

  out.ensureAvailableCapacityExponential(1UL + URLEncodedSize(decodedKey, isUnreserved) + 1UL +
                                         URLEncodedSize(decodedValue, isUnreserved));

  char* pEnd = out.data() + out.size();

  *pEnd++ = leadingChar;
  pEnd = URLEncode(decodedKey, isUnreserved, pEnd);
  *pEnd++ = '=';
  pEnd = URLEncode(decodedValue, isUnreserved, pEnd);

  out.setEnd(pEnd);
}

bool BuildHttpsRedirectUrl(RawChars& out, const HttpRequestView& request, uint16_t targetPort) {
#ifdef AERONET_ENABLE_HTTP2
  // :authority replaces the Host header in HTTP/2 (RFC 9113 §8.3.1), which may still send one instead.
  const std::string_view authority = request.authority();
  const std::string_view hostHeader = authority.empty() ? request.headerValueOrEmpty(Host) : authority;
#else
  const std::string_view hostHeader = request.headerValueOrEmpty(Host);
#endif
  if (!SetHttpsAuthority(out, hostHeader, targetPort)) {
    return false;
  }
  AppendUrlEncodedPath(out, request.path());
  char querySep = '?';
  for (const auto& [key, value] : request.queryParamsRange()) {
    AppendUrlEncodedQueryParam(out, querySep, key, value);
    querySep = '&';
  }
  return true;
}

}  // namespace aeronet::http
