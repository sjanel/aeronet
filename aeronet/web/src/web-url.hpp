#pragma once

#include <cstddef>
#include <string_view>

namespace aeronet::internal {

// The components of a URL serialized by the URL parser (UrlResolver), as the Location and URL interfaces name them.
struct UrlParts {
  std::string_view protocol;  // "https:"
  std::string_view host;      // "example.com:8443"
  std::string_view hostname;  // "example.com", "[::1]"
  std::string_view port;      // "8443", empty for the default port
  std::string_view pathname;  // "/a/b"
  std::string_view search;    // "?q=1", empty without a query or for an empty one
  std::string_view hash;      // "#top", empty without a fragment or for an empty one
};

// The components of 'url', a serialized URL: its authority has no user information (http and https URLs).
constexpr UrlParts SplitUrl(std::string_view url) noexcept {
  UrlParts parts;
  const std::size_t colon = url.find(':');
  parts.protocol = url.substr(0, colon + 1);
  std::string_view rest = url.substr(colon + 1);
  if (rest.starts_with("//")) {
    rest.remove_prefix(2);
    const std::size_t authorityEnd = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, authorityEnd);
    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
      authority.remove_prefix(at + 1);
    }
    parts.host = authority;
    const std::size_t portColon = authority.rfind(':');
    const std::size_t bracket = authority.rfind(']');
    if (portColon != std::string_view::npos && (bracket == std::string_view::npos || portColon > bracket)) {
      parts.hostname = authority.substr(0, portColon);
      parts.port = authority.substr(portColon + 1);
    } else {
      parts.hostname = authority;
    }
    rest = authorityEnd == std::string_view::npos ? std::string_view{} : rest.substr(authorityEnd);
  }
  if (const std::size_t hashPos = rest.find('#'); hashPos != std::string_view::npos) {
    parts.hash = rest.substr(hashPos);
    rest = rest.substr(0, hashPos);
  }
  if (const std::size_t queryPos = rest.find('?'); queryPos != std::string_view::npos) {
    parts.search = rest.substr(queryPos);
    rest = rest.substr(0, queryPos);
  }
  parts.pathname = rest;
  if (parts.hash.size() == 1) {
    parts.hash = {};
  }
  if (parts.search.size() == 1) {
    parts.search = {};
  }
  return parts;
}

constexpr bool IsHttpUrl(std::string_view url) noexcept {
  return url.starts_with("http://") || url.starts_with("https://");
}

// Whether the serialized http or https URL 'url' is of the origin 'origin' (the prefix of a URL without user
// information, "https://example.com:8443").
constexpr bool IsOfOrigin(std::string_view url, std::string_view origin) noexcept {
  // The authority of a serialized http or https URL is followed by its path.
  return url.size() > origin.size() && url.starts_with(origin) && url[origin.size()] == '/';
}

// The origin of 'url', a serialized http or https URL without user information ("https://example.com:8443"): its prefix
// before its path.
constexpr std::string_view OriginOf(std::string_view url) noexcept {
  const std::size_t authorityPos = url.find("//") + 2U;
  return url.substr(0, url.find_first_of("/?#", authorityPos));
}

// 'url' without its fragment.
constexpr std::string_view WithoutFragment(std::string_view url) noexcept { return url.substr(0, url.find('#')); }

}  // namespace aeronet::internal
