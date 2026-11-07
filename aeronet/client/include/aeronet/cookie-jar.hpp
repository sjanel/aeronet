#pragma once

#include <amc/type_traits.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "aeronet/city-hash.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

class HttpClient;
class HttpRequest;
class HttpResponse;

// The SameSite attribute of a cookie (RFC 6265bis §4.1.2.7). Default: absent or unknown value, sent like None.
enum class CookieSameSite : uint8_t { Default, None, Lax, Strict };

namespace internal {

// How a request relates to the requests of its redirect chain (RFC 6265bis §5.2). HttpClient requests are navigations,
// so SameSite=Lax cookies are still sent after a redirect to another site, but only with a safe method.
enum class CookieSiteContext : uint8_t {
  SameSite,         // every URL of the chain is of the same site
  CrossSiteSafe,    // the chain changed site, safe method (GET, HEAD...): no SameSite=Strict cookie
  CrossSiteUnsafe,  // the chain changed site, unsafe method (POST...): no SameSite=Strict or Lax cookie
};

// The URL of a request, as the cookie rules see it.
struct CookieRequestUrl {
  std::string_view host;  // as received, any case
  std::string_view path;  // without the query
  bool https;
};

}  // namespace internal

// A cookie of a CookieJar. Its views point into the jar: they stay valid until the jar is modified.
struct Cookie {
  // Whether the cookie outlives the session: it has an Expires or a Max-Age attribute.
  [[nodiscard]] bool persistent() const noexcept { return expiry != SysTimePoint::max(); }

  std::string_view name;
  std::string_view value;
  // Lower-case: the host that set a host-only cookie, else its Domain attribute without leading dot.
  std::string_view domain;
  std::string_view path;
  SysTimePoint expiry;  // SysTimePoint::max() for a session cookie
  bool hostOnly;        // sent to 'domain' only, not to its subdomains (no Domain attribute)
  bool secure;          // sent to https URLs only, and to loopback hosts (localhost, 127.0.0.1, ::1) as browsers do
  bool httpOnly;        // hidden from page scripts: no effect on HttpClient, which sends it
  CookieSameSite sameSite;
};

// The cookie storage of an HTTP user agent (RFC 6265bis): the cookies set by the Set-Cookie headers of the responses
// are sent back in the Cookie header of the next requests to the URLs they match. HttpClient owns one, which it uses
// when HttpClientConfig::cookies is enabled (see HttpClient::cookieJar()).
//
// Without the public suffix list, the registrable domain of a host (example.com for www.example.com) is estimated from
// its last labels: a Domain attribute naming a top-level domain ("com") or a country-code second-level one ("co.uk",
// "ne.jp") is refused, but private suffixes ("github.io") are not known. Two URLs are of the same site when they have
// the same scheme and host, which withholds SameSite=Strict cookies after a redirect to another subdomain.
//
// Not thread-safe. The cookies live in memory only.
class CookieJar {
 public:
  // A jar with the default limits of HttpClientConfig::Cookies: at most 3000 cookies, and 180 per registrable domain.
  CookieJar() noexcept = default;

  // A jar of at most 'maxCookies' cookies, and 'maxCookiesPerDomain' per registrable domain (both > 0). When a limit is
  // exceeded, the expired cookies are removed, then the least recently sent ones, down to the limit minus a tenth.
  CookieJar(uint32_t maxCookies, uint32_t maxCookiesPerDomain) noexcept;

  CookieJar(const CookieJar&) = default;
  CookieJar& operator=(const CookieJar&) = default;

  // The moved-from jar is empty, with its limits.
  CookieJar(CookieJar&& rhs) noexcept;
  CookieJar& operator=(CookieJar&& rhs) noexcept;

  ~CookieJar() = default;

  // Stores the cookie of 'setCookieValue', the value of a Set-Cookie header received in a response to 'url' (an http or
  // https URL), as a browser does for a navigation (RFC 6265bis §5.6, §5.7): a cookie of the same name, domain and path
  // is replaced, and an expired one (Max-Age <= 0, or Expires in the past) deletes it. Returns false when the cookie is
  // refused: malformed, too large (4096 bytes of name and value), Domain not matching the host of 'url' or being a
  // public suffix, Secure (or a __Secure- / __Host- name) from an http URL, or shadowing a Secure cookie from an http
  // URL. Throws std::invalid_argument for an invalid URL.
  [[nodiscard]] bool setCookie(std::string_view url, std::string_view setCookieValue,
                               SysTimePoint now = SysClock::now());

  // The value of the Cookie header of a request to 'url' (an http or https URL): the cookies matching its host and
  // path, not expired, Secure ones only for https and loopback hosts, the longest paths first. Empty when no cookie
  // matches. The cookies are not marked as sent: the order of their eviction does not change. Throws
  // std::invalid_argument for an invalid URL.
  [[nodiscard]] std::string cookieHeader(std::string_view url, SysTimePoint now = SysClock::now()) const;

  // Calls 'callback(const Cookie&)' for each cookie, including the expired ones not removed yet.
  template <class Callback>
  void forEach(Callback&& callback) const {
    for (const auto& [domainKey, cookies] : _cookiesByDomain) {
      for (const StoredCookie& cookie : cookies) {
        std::invoke(callback, cookie.view());
      }
    }
  }

  // Removes the expired cookies, and releases the memory that no cookie uses anymore.
  void removeExpired(SysTimePoint now = SysClock::now());

  // Removes every cookie.
  void clear() noexcept;

  // Number of cookies, including the expired ones not removed yet.
  [[nodiscard]] std::size_t size() const noexcept { return _nbCookies; }

  [[nodiscard]] bool empty() const noexcept { return _nbCookies == 0; }

 private:
  friend class HttpClient;
  friend class CookieJarTest;

  struct StoredCookie {
    [[nodiscard]] std::string_view name() const noexcept { return {data.data(), nameLen}; }

    [[nodiscard]] std::string_view value() const noexcept { return {data.data() + nameLen, valueLen}; }

    [[nodiscard]] std::string_view domain() const noexcept { return {data.data() + nameLen + valueLen, domainLen}; }

    [[nodiscard]] std::string_view path() const noexcept {
      const std::size_t pathPos = static_cast<std::size_t>(nameLen) + valueLen + domainLen;
      return {data.data() + pathPos, data.size() - pathPos};
    }

    [[nodiscard]] Cookie view() const noexcept {
      return {name(), value(), domain(), path(), expiry, hostOnly, secure, httpOnly, sameSite};
    }

    using trivially_relocatable = amc::is_trivially_relocatable<RawChars32>::type;

    // An allocation per cookie, which its replacements reuse: an arena such as ObjectArrayPool cannot free the
    // replaced, deleted and evicted cookies one by one.
    RawChars32 data;  // name, value, domain and path, in a single allocation
    SysTimePoint expiry;
    uint64_t lastAccess;  // jar sequence number of the last storage or sending
    uint16_t nameLen;
    uint16_t valueLen;
    uint16_t domainLen;
    bool hostOnly : 1;
    bool secure : 1;
    bool httpOnly : 1;
    CookieSameSite sameSite;
  };

  static_assert(sizeof(void*) != 8 || sizeof(StoredCookie) == 40, "keep StoredCookie compact");

  // Cookies of one registrable domain, sorted by decreasing path length, then by creation: the order of the Cookie
  // header (RFC 6265bis §5.8.3), so that it is built in a single pass. The creation order needs no field: a new cookie
  // goes after those of a path not shorter, and a replacement keeps its position.
  using Cookies = vector<StoredCookie>;

  // Stores the cookie of 'setCookieValue' received from 'url'. Returns false when it is refused.
  bool store(internal::CookieRequestUrl url, std::string_view setCookieValue, SysTimePoint now);

  // Appends to 'out' the cookie-pairs of a request to 'url' from 'jar', separated by "; ". A non-const 'jar' marks them
  // as just sent, for the eviction of the least recently used cookies.
  template <class Jar>
  static void AppendCookies(Jar& jar, internal::CookieRequestUrl url, internal::CookieSiteContext context,
                            SysTimePoint now, RawChars& out);

  // Stores the cookies of the Set-Cookie headers of 'response', received for 'request'.
  void storeResponseCookies(const HttpRequest& request, const HttpResponse& response, SysTimePoint now);

  // Sets requestCookies() to the Cookie header value of 'request', whose cookies are marked as sent.
  void prepareRequestCookies(const HttpRequest& request, internal::CookieSiteContext context, SysTimePoint now);

  // Sets requestCookies() to the Cookie header value that the first exchange of 'request' would send, for its cache
  // key, without marking the cookies as sent: a cached response sends none.
  void prepareCacheKeyCookies(const HttpRequest& request, SysTimePoint now);

  // The Cookie header value prepared by the last prepareRequestCookies() or prepareCacheKeyCookies() call, which the
  // protocol engines send. Empty when the client does not use the jar.
  [[nodiscard]] std::string_view requestCookies() const noexcept { return _requestCookies; }

  // Removes the expired cookies of every domain, and the domains left without cookie.
  void eraseExpired(SysTimePoint now);

  // Removes the expired cookies of 'pCookies' (of every domain when null), then the least recently used ones down to
  // 'maxCount' minus a tenth, if more than 'maxCount' remain: evicting several at once amortizes the cost of a full
  // jar.
  void evict(Cookies* pCookies, std::size_t maxCount, SysTimePoint now);

  // Cookies by registrable domain (or host, for an IP address or a public suffix such as localhost), lower-case.
  flat_hash_map<RawChars32, Cookies, CityHash, std::equal_to<>> _cookiesByDomain;
  RawChars _requestCookies;
  uint64_t _sequence{0};
  uint32_t _nbCookies{0};
  uint32_t _maxCookies{HttpClientConfig::Cookies::kDefaultMaxCookies};
  uint32_t _maxCookiesPerDomain{HttpClientConfig::Cookies::kDefaultMaxCookiesPerDomain};
};

}  // namespace aeronet
