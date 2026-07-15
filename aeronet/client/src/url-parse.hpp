#pragma once

#include <charconv>
#include <string_view>
#include <system_error>

#include "aeronet/internal/url-parsed-result.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet::internal {

inline constexpr std::string_view kHttp = "http";
inline constexpr std::string_view kHttps = "https";

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

// Parse the "host[:port][/path][?query]" slice that follows "scheme://" (or a "//" network-path
// reference) for an already-resolved scheme.
// Returns an empty host on malformed input.
constexpr void ParseAuthority(std::string_view rest, UrlParseResult& res) {
  // Strip fragment (never transmitted).
  if (const auto hashPos = rest.find('#'); hashPos != std::string_view::npos) {
    rest = rest.substr(0, hashPos);
  }

  // Authority ends at the first '/' or '?'.
  const auto authorityEnd = rest.find_first_of("/?");
  const std::string_view authority = rest.substr(0, authorityEnd);

  res.target = authorityEnd == std::string_view::npos ? std::string_view{"/"} : rest.substr(authorityEnd);

  if (authority.empty() || authority.contains('@')) {
    return;  // empty authority or unsupported userinfo
  }

  std::string_view portPart;
  if (authority.front() == '[') {
    // IPv6 literal: [::1]:port
    const auto close = authority.find(']');
    if (close == std::string_view::npos) {
      return;
    }
    const std::string_view afterBracket = authority.substr(close + 1);
    if (!afterBracket.empty()) {
      if (afterBracket.front() != ':') {
        return;
      }
      portPart = afterBracket.substr(1);
    }
    res.host = authority.substr(1, close - 1);
  } else {
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
      res.host = authority;
    } else {
      res.host = authority.substr(0, colon);
      portPart = authority.substr(colon + 1);
    }
  }

  if (res.host.empty()) {
    return;
  }

  if (portPart.empty()) {
    res.port = res.isTls ? 443 : 80;
  } else {
    const auto* begin = portPart.data();
    const auto* end = begin + portPart.size();
    const auto [ptr, ec] = std::from_chars(begin, end, res.port);
    if (ec != std::errc{} || ptr != end || res.port == 0) {
      res.host = {};
      return;
    }
  }
}

// Returns an empty host on malformed input.
constexpr UrlParseResult ParseUrl(std::string_view url) {
  UrlParseResult res;

  const auto schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos || schemeEnd == 0) {
    return res;
  }

  if (!internal::IsHttpScheme(url.substr(0, schemeEnd), res.isTls)) {
    return res;
  }

  // Strip fragment (never transmitted).
  std::string_view rest = url.substr(schemeEnd + 3);

  ParseAuthority(rest, res);

  return res;
}

}  // namespace aeronet::internal