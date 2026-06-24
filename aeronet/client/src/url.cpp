#include "aeronet/url.hpp"

#include <charconv>
#include <cstdint>
#include <expected>
#include <string_view>
#include <system_error>

#include "aeronet/http-client-error.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet {

namespace {

constexpr std::string_view kHttp = "http";
constexpr std::string_view kHttps = "https";

constexpr bool IsHttpScheme(std::string_view scheme, bool& tls) {
  if (CaseInsensitiveEqual(kHttps, scheme)) {
    tls = true;
    return true;
  }
  if (CaseInsensitiveEqual(kHttp, scheme)) {
    tls = false;
    return true;
  }
  return false;
}

struct ParsedAuthority {
  std::string_view host;
  std::string_view target;  // origin-form target; "/" when the URL carries no path
  uint16_t port{};
};

// Parse the "host[:port][/path][?query]" slice that follows "scheme://" (or a "//" network-path
// reference) for an already-resolved scheme.
// Returns an empty host on malformed input.
ParsedAuthority ParseAuthority(bool tls, std::string_view rest) {
  ParsedAuthority pa;

  // Strip fragment (never transmitted).
  if (const auto hashPos = rest.find('#'); hashPos != std::string_view::npos) {
    rest = rest.substr(0, hashPos);
  }

  // Authority ends at the first '/' or '?'.
  const auto authorityEnd = rest.find_first_of("/?");
  const std::string_view authority = rest.substr(0, authorityEnd);

  pa.target = authorityEnd == std::string_view::npos ? std::string_view{"/"} : rest.substr(authorityEnd);

  if (authority.empty() || authority.contains('@')) {
    return pa;  // empty authority or unsupported userinfo
  }

  std::string_view portPart;
  if (authority.front() == '[') {
    // IPv6 literal: [::1]:port
    const auto close = authority.find(']');
    if (close == std::string_view::npos) {
      return pa;
    }
    const std::string_view afterBracket = authority.substr(close + 1);
    if (!afterBracket.empty()) {
      if (afterBracket.front() != ':') {
        return pa;
      }
      portPart = afterBracket.substr(1);
    }
    pa.host = authority.substr(1, close - 1);
  } else {
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
      pa.host = authority;
    } else {
      pa.host = authority.substr(0, colon);
      portPart = authority.substr(colon + 1);
    }
  }

  if (pa.host.empty()) {
    return pa;
  }

  if (portPart.empty()) {
    pa.port = tls ? 443 : 80;
  } else {
    const auto* begin = portPart.data();
    const auto* end = begin + portPart.size();
    const auto [ptr, ec] = std::from_chars(begin, end, pa.port);
    if (ec != std::errc{} || ptr != end || pa.port == 0) {
      pa.host = {};
      return pa;
    }
  }

  return pa;
}

}  // namespace

void Url::buildCanonical(bool tls, uint16_t port, std::string_view host, std::string_view target) {
  const std::string_view scheme = tls ? kHttps : kHttp;
  const auto portLen = ndigits(port);
  _buf = RawChars32(scheme.size() + kSchemeSep.size() + host.size() + 1U + portLen + target.size());

  char* pEnd = Append(scheme, _buf.data());
  pEnd = Append(kSchemeSep, pEnd);
  pEnd = Append(host, pEnd);
  *pEnd++ = ':';
  pEnd = std::to_chars(pEnd, pEnd + portLen, port).ptr;

  _schemeLen = SafeCast<uint8_t>(scheme.size());
  _hostLen = SafeCast<uint16_t>(host.size());
  _originKeyLen = SafeCast<uint16_t>(pEnd - _buf.data());
  _port = port;

  pEnd = Append(target, pEnd);
  _buf.setSize(SafeCast<RawChars32::size_type>(pEnd - _buf.data()));
}

std::expected<Url, HttpClientErrc> Url::Parse(std::string_view url) {
  const auto schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos || schemeEnd == 0) {
    return std::unexpected(HttpClientErrc::invalidUrl);
  }
  bool tls = false;
  if (!IsHttpScheme(url.substr(0, schemeEnd), tls)) {
    return std::unexpected(HttpClientErrc::invalidUrl);
  }
  const ParsedAuthority pa = ParseAuthority(tls, url.substr(schemeEnd + 3));
  if (pa.host.empty()) {
    return std::unexpected(HttpClientErrc::invalidUrl);
  }
  return Url(tls, pa.port, pa.host, pa.target);
}

std::expected<Url, HttpClientErrc> Url::resolveRedirect(std::string_view location) const {
  if (location.empty()) {
    return std::unexpected(HttpClientErrc::invalidUrl);
  }

  // Absolute URL. A malformed target surfaces as HttpClientErrc::invalidUrl (propagated out of the
  // request); it is not expected on a nominal redirect.
  if (location.contains("://")) {
    return Parse(location);
  }
  // Network-path reference: //host[:port][/path] -> inherit scheme. Parse the authority directly and
  // build the canonical buffer once, instead of synthesizing a "scheme://..." string to re-parse and copy.
  if (location.starts_with("//")) {
    const ParsedAuthority pa = ParseAuthority(tls(), location.substr(2));
    if (pa.host.empty()) {
      return std::unexpected(HttpClientErrc::invalidUrl);
    }
    return Url(tls(), pa.port, pa.host, pa.target);
  }

  // Strip fragment from the relative reference.
  if (const auto hashPos = location.find('#'); hashPos != std::string_view::npos) {
    location = location.substr(0, hashPos);
  }

  if (location.starts_with('/')) {
    // Absolute path: keep origin, replace target.
    return Url(tls(), _port, host(), location);
  }

  // Relative path: resolve against the directory portion of the current target.
  std::string_view base = target();
  if (const auto questionMarkPos = base.find('?'); questionMarkPos != std::string_view::npos) {
    base = base.substr(0, questionMarkPos);
  }
  const auto lastSlash = base.rfind('/');
  std::string_view dir = lastSlash == std::string_view::npos ? std::string_view{"/"} : base.substr(0, lastSlash + 1);

  RawChars resolved(dir.size() + location.size());
  resolved.unchecked_append(dir);
  resolved.unchecked_append(location);
  return Url(tls(), _port, host(), resolved);
}

}  // namespace aeronet
