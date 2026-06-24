#pragma once

#include <amc/type_traits.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

#include "aeronet/http-client-error.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

// Parsed absolute HTTP/HTTPS URL.
//
// Only the subset needed by the HTTP client is extracted (no userinfo, no fragment in the
// request target). The whole URL is materialized as a single contiguous buffer holding the
// canonical form "scheme://host:port" immediately followed by the request-target:
//
//   "https://example.com:443/path?query"
//    \____/  \_________/ \_/ \__________/
//    scheme     host    port    target
//    \________________________/
//             originKey
//
// The origin key ("scheme://host:port") is therefore the contiguous prefix [0, originKey length)
// and the request-target is the suffix; both are zero-cost string_views into the same allocation.
// Small length fields make every component an O(1) slice. The object is small, trivially relocatable
// and costs a single heap allocation. The host is stored unbracketed even for IPv6 literals.
class Url {
 public:
  // RAII helper returned by hostCStr(): guarantees a null-terminated host C-string for its lifetime
  // by temporarily overwriting the ':' separator that follows the host with '\0', restoring it on
  // destruction (the same trick as CharReplacer in tcp-connector.cpp). While alive, the backing
  // buffer is transiently mutated, so originKey()/port-string must not be read concurrently.
  class HostCStr {
   public:
    HostCStr(char* host, std::size_t hostLen) : _sep(host + hostLen), _saved(*_sep), _host(host) { *_sep = '\0'; }

    HostCStr(const HostCStr&) = delete;
    HostCStr(HostCStr&&) = delete;
    HostCStr& operator=(const HostCStr&) = delete;
    HostCStr& operator=(HostCStr&&) = delete;

    ~HostCStr() { *_sep = _saved; }

    [[nodiscard]] const char* c_str() const noexcept { return _host; }

   private:
    char* _sep;
    char _saved;
    const char* _host;
  };

  // Parse an absolute URL of the form: scheme://host[:port][/path][?query][#fragment], returning the parsed
  // Url or HttpClientErrc::invalidUrl when the URL is malformed or the scheme is not http/https (never
  // throws). The fragment (#...) is stripped and never sent to the server.
  [[nodiscard]] static std::expected<Url, HttpClientErrc> Parse(std::string_view url);

  // Resolve a (possibly relative) Location header value against this URL, returning the absolute target to
  // follow for a redirect, or HttpClientErrc::invalidUrl when the location is empty or resolves to a
  // malformed/unsupported URL (never throws). Supports absolute URLs, network-path references
  // (//host/path), absolute paths (/path) and relative paths (resolved against the current dir).
  [[nodiscard]] std::expected<Url, HttpClientErrc> resolveRedirect(std::string_view location) const;

  [[nodiscard]] bool tls() const noexcept { return _schemeLen == 5; }  // "https" == 5, "http" == 4
  [[nodiscard]] uint16_t port() const noexcept { return _port; }

  // "http" or "https".
  [[nodiscard]] std::string_view scheme() const noexcept { return {_buf.data(), _schemeLen}; }

  // Registered name or IP literal (no brackets for IPv6).
  [[nodiscard]] std::string_view host() const noexcept {
    return {_buf.data() + _schemeLen + kSchemeSep.size(), _hostLen};
  }
  // Null-terminated host C-string, valid for the lifetime of the returned guard (see HostCStr).
  [[nodiscard]] HostCStr hostCStr() const noexcept {
    // const_cast: _buf is the Url's own mutable heap storage; the guard restores the byte it changes.
    return {const_cast<char*>(host().data()), _hostLen};
  }

  // Connection-pool origin key: the contiguous prefix "scheme://host:port".
  [[nodiscard]] std::string_view originKey() const noexcept { return {_buf.data(), _originKeyLen}; }

  // Origin-form request target: path + optional "?query" (never empty -> "/").
  [[nodiscard]] std::string_view target() const noexcept {
    return {_buf.data() + _originKeyLen, _buf.size() - _originKeyLen};
  }

  // True when the port is the scheme default (80 for http, 443 for https): the Host header then omits it.
  [[nodiscard]] bool isDefaultPort() const noexcept { return _port == (tls() ? 443 : 80); }

  using trivially_relocatable = amc::is_trivially_relocatable<RawChars32>::type;

 private:
  static constexpr std::string_view kSchemeSep = "://";

  // Construct from already-validated components; used by resolveRedirect to retarget the current origin.
  Url(bool tls, uint16_t port, std::string_view host, std::string_view target) {
    buildCanonical(tls, port, host, target);
  }

  // Materialize the canonical "scheme://host:port" + target buffer and set the slice fields in place.
  // Shared by both constructors so the parsing one fills its fields directly instead of building and
  // move-assigning a throwaway Url.
  void buildCanonical(bool tls, uint16_t port, std::string_view host, std::string_view target);

  RawChars32 _buf;         // "scheme://host:port" + target (single allocation)
  uint16_t _originKeyLen;  // length of "scheme://host:port" == offset of target
  uint16_t _hostLen;       // length of host
  uint16_t _port;          // parsed port
  uint8_t _schemeLen;      // 4 ("http") or 5 ("https")
};

}  // namespace aeronet
