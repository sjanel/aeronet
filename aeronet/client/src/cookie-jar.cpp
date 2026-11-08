#include "aeronet/cookie-jar.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/cctype.hpp"
#include "aeronet/compiler-config.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/internal/url-parsed-result.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"
#include "aeronet/tolower-str.hpp"
#include "aeronet/toupperlower.hpp"
#include "aeronet/vector.hpp"
#include "url-parse.hpp"

namespace aeronet {

namespace {

// Limits of a Set-Cookie header (RFC 6265bis §5.6).
constexpr std::size_t kMaxNameValueSize = 4096;
constexpr std::size_t kMaxAttributeValueSize = 1024;

// Longest attribute name the parser knows ("httponly", "samesite"): a longer one is unknown, so ignored.
constexpr std::size_t kMaxAttributeNameSize = 8;

// Longest host name (RFC 1035 allows 253 characters): a longer host gets no cookie.
constexpr std::size_t kMaxHostSize = 255;

// Longest lifetime of a cookie (RFC 6265bis §5.6.1, §5.6.2).
constexpr std::chrono::seconds kMaxLifetime = std::chrono::days{400};

constexpr SysTimePoint kSessionExpiry = SysTimePoint::max();
constexpr SysTimePoint kExpired = SysTimePoint::min();

// The case-insensitive cookie name prefixes (RFC 6265bis §4.1.3), in lower-case.
constexpr std::string_view kSecureNamePrefix = "__secure-";
constexpr std::string_view kHostNamePrefix = "__host-";

// 'str' in lower-case, copied to 'buf': the case-insensitive parts of the cookies are lower-cased once, then compared
// exactly. Empty when 'str' is longer than 'buf', which is sized for the longest string that can be of use.
constexpr std::string_view LowerCaseInto(std::string_view str, std::span<char> buf) noexcept {
  if (str.size() > buf.size()) {
    return {};
  }
  tolower_n(str.data(), str.size(), buf.data());
  return {buf.data(), str.size()};
}

// Whether 'host' is an IP address: an IPv6 one has colons (its brackets are removed from URL hosts), and as no
// top-level domain is numeric, a last label of digits makes an IPv4 one.
constexpr bool IsIpAddress(std::string_view host) noexcept {
  if (host.contains(':')) {
    return true;
  }
  const std::string_view lastLabel = host.substr(host.rfind('.') + 1);
  return !lastLabel.empty() && std::ranges::all_of(lastLabel, isdigit);
}

// Whether the lower-case 'host' is a loopback host, a secure context like an https origin for the cookies, as in
// browsers.
constexpr bool IsLoopbackHost(std::string_view host) noexcept {
  return host == "localhost" || host.ends_with(".localhost") || host == "::1" ||
         (host.starts_with("127.") && IsIpAddress(host));
}

// Second-level labels under which country-code top-level domains register names (co.uk, com.au, ne.jp...). Without the
// public suffix list, two labels of this form are a public suffix, as in Python's http.cookiejar.
constexpr std::string_view kCountryCodeSecondLevels[]{
    "ac", "ad", "co", "com", "ed", "edu", "go", "gov", "gr", "lg", "mil", "ne", "net", "or", "org",
};

// The registrable domain of the lower-case 'host': example.com for www.example.com, example.co.uk for
// www.example.co.uk. Empty when 'host' is itself a public suffix: a single label ("com", "localhost") or a country-code
// second-level domain ("co.uk"). An IP address is its own registrable domain.
std::string_view RegistrableDomain(std::string_view host) noexcept {
  if (IsIpAddress(host)) {
    return host;
  }
  const std::size_t lastDot = host.rfind('.');
  if (lastDot == std::string_view::npos || lastDot == 0) {
    return {};
  }
  // Start of the last two labels, npos when the host has only two.
  std::size_t dotBefore = host.rfind('.', lastDot - 1);
  const std::size_t secondLevelPos = dotBefore == std::string_view::npos ? 0 : dotBefore + 1;
  const std::string_view secondLevel = host.substr(secondLevelPos, lastDot - secondLevelPos);
  if (host.size() - lastDot - 1 == 2 && std::ranges::contains(kCountryCodeSecondLevels, secondLevel)) {
    if (dotBefore == std::string_view::npos || dotBefore == 0) {
      return {};
    }
    dotBefore = host.rfind('.', dotBefore - 1);
  }
  return dotBefore == std::string_view::npos ? host : host.substr(dotBefore + 1);
}

// The key of the cookies of the lower-case 'host' in the jar: its registrable domain, or itself when it is a public
// suffix (it can only have host-only cookies then). A cookie domain matching 'host', never a public suffix, has the
// same registrable domain: all the cookies of a request are under one key.
std::string_view DomainKey(std::string_view host) noexcept {
  const std::string_view registrableDomain = RegistrableDomain(host);
  return registrableDomain.empty() ? host : registrableDomain;
}

// Whether the lower-case 'host' domain-matches the lower-case 'domain' (RFC 6265bis §5.1.3).
constexpr bool DomainMatch(std::string_view host, std::string_view domain) noexcept {
  return host == domain || (host.size() > domain.size() && host.ends_with(domain) &&
                            host[host.size() - domain.size() - 1] == '.' && !IsIpAddress(host));
}

// Whether 'requestPath' path-matches 'cookiePath' (RFC 6265bis §5.1.4).
constexpr bool PathMatch(std::string_view requestPath, std::string_view cookiePath) noexcept {
  return requestPath.starts_with(cookiePath) && (requestPath.size() == cookiePath.size() || cookiePath.ends_with('/') ||
                                                 requestPath[cookiePath.size()] == '/');
}

// The path of a cookie without Path attribute (RFC 6265bis §5.1.4): the directory of the request path, which starts
// with a slash (see TargetPath()).
constexpr std::string_view DefaultPath(std::string_view requestPath) noexcept {
  assert(requestPath.starts_with('/'));
  const std::size_t lastSlash = requestPath.rfind('/');
  return lastSlash == 0 ? std::string_view{"/"} : requestPath.substr(0, lastSlash);
}

// The path of a request target, without its query: the target is in origin-form ("/path?query", or "?query" for a URL
// without path), or in absolute-form ("http://host:port/path?query") for a request sent to a forward proxy. An empty
// path is "/" (RFC 9110 §4.2.3).
constexpr std::string_view TargetPath(std::string_view target) noexcept {
  if (!target.starts_with('/') && !target.starts_with('?')) {
    // The path of the absolute-form starts at the first '/' or '?' after the scheme and the authority, which hold none.
    assert(target.contains("://"));
    target.remove_prefix(std::min(target.find_first_of("/?", target.find("://") + 3), target.size()));
  }
  const std::string_view path = target.substr(0, target.find('?'));
  return path.empty() ? std::string_view{"/"} : path;
}

// Whether a cookie of this SameSite attribute goes with a request of this context (RFC 6265bis §5.8.3). A cookie
// without the attribute is sent like SameSite=None, as Firefox and Safari do.
constexpr bool SameSiteAllows(CookieSameSite sameSite, internal::CookieSiteContext context) noexcept {
  switch (sameSite) {
    case CookieSameSite::Strict:
      return context == internal::CookieSiteContext::SameSite;
    case CookieSameSite::Lax:
      return context != internal::CookieSiteContext::CrossSiteUnsafe;
    default:
      return true;
  }
}

// --- cookie-date (RFC 6265bis §5.1.1) ---

// The delimiters are a tab and the punctuation characters. The other control characters, non-delimiters, never come
// here: a Set-Cookie header holding one is refused whole (see ParseSetCookie).
constexpr bool IsDateDelimiter(char ch) noexcept {
  const unsigned char byte = static_cast<unsigned char>(ch);
  return byte <= 0x2FU || (byte >= 0x3BU && byte <= 0x40U) || (byte >= 0x5BU && byte <= 0x60U) ||
         (byte >= 0x7BU && byte <= 0x7EU);
}

// The number made of 'minDigits' to 'maxDigits' digits at the start of 'token', which must then end or go on with a
// non-digit. -1 when there is no such number.
constexpr int LeadingNumber(std::string_view token, std::size_t minDigits, std::size_t maxDigits) noexcept {
  int value = 0;
  std::size_t nbDigits = 0;
  for (; nbDigits < token.size() && isdigit(token[nbDigits]); ++nbDigits) {
    if (nbDigits == maxDigits) {
      return -1;
    }
    value = (value * 10) + (token[nbDigits] - '0');
  }
  return nbDigits < minDigits ? -1 : value;
}

// Parses the hms-time "h:m:s" (1 or 2 digits each) at the start of 'token', which may go on after a non-digit. Returns
// false, without changing the outputs, when it is not one.
constexpr bool ParseTime(std::string_view token, int& hour, int& minute, int& second) noexcept {
  const std::size_t firstColon = token.find(':');
  const std::size_t secondColon =
      firstColon == std::string_view::npos ? std::string_view::npos : token.find(':', firstColon + 1);
  if (secondColon == std::string_view::npos) {
    return false;
  }
  // The hours and minutes are digits only, between their colons.
  const auto fieldValue = [](std::string_view field) {
    return std::ranges::all_of(field, isdigit) ? LeadingNumber(field, 1, 2) : -1;
  };
  const int hourValue = fieldValue(token.substr(0, firstColon));
  const int minuteValue = fieldValue(token.substr(firstColon + 1, secondColon - firstColon - 1));
  const int secondValue = LeadingNumber(token.substr(secondColon + 1), 1, 2);
  if (hourValue < 0 || minuteValue < 0 || secondValue < 0) {
    return false;
  }
  hour = hourValue;
  minute = minuteValue;
  second = secondValue;
  return true;
}

// The month (1 to 12) whose abbreviation starts 'token' in any case ("jan", "JANUARY"), 0 when there is none.
constexpr unsigned ParseMonth(std::string_view token) noexcept {
  if (token.size() < 3) {
    return 0;
  }
  // Capitalized like the abbreviations of the HTTP dates.
  const char capitalized[]{toupper(token[0]), tolower(token[1]), tolower(token[2])};
  return MonthOfAbbreviation(capitalized);
}

// Parses the value of an Expires attribute with the lenient algorithm of RFC 6265bis §5.1.1, which reads the formats
// servers send ("Wed, 21 Oct 2026 07:28:00 GMT", "Wednesday, 21-Oct-26 07:28:00 GMT", "Wed Oct 21 07:28:00 2026").
// Returns false when it is not a date.
bool ParseCookieDate(std::string_view value, std::chrono::sys_seconds& date) noexcept {
  int hour = -1;
  int minute = 0;
  int second = 0;
  int day = -1;
  unsigned month = 0;
  int year = -1;
  for (;;) {
    value.remove_prefix(static_cast<std::size_t>(std::ranges::find_if_not(value, IsDateDelimiter) - value.begin()));
    if (value.empty()) {
      break;
    }
    const std::string_view token(value.begin(), std::ranges::find_if(value, IsDateDelimiter));
    value.remove_prefix(token.size());
    if (hour < 0 && ParseTime(token, hour, minute, second)) {
      continue;
    }
    if (day < 0) {
      day = LeadingNumber(token, 1, 2);
      if (day >= 0) {
        continue;
      }
    }
    if (month == 0) {
      month = ParseMonth(token);
      if (month != 0) {
        continue;
      }
    }
    if (year < 0) {
      year = LeadingNumber(token, 2, 4);
    }
  }
  if (year >= 70 && year <= 99) {
    year += 1900;
  } else if (year >= 0 && year <= 69) {
    year += 2000;
  }
  if (hour < 0 || day < 1 || day > 31 || month == 0 || year < 1601 || hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  const std::chrono::year_month_day ymd{std::chrono::year{year}, std::chrono::month{month},
                                        std::chrono::day{static_cast<unsigned>(day)}};
  if (!ymd.ok()) {
    return false;  // such as February 30
  }
  // In seconds from the start: the hours and minutes of MSVC are 32-bit counts, and the minutes since 1970 overflow one
  // after year 6053.
  date = std::chrono::sys_seconds{std::chrono::sys_days{ymd}} + std::chrono::hours{hour} +
         std::chrono::minutes{minute} + std::chrono::seconds{second};
  return true;
}

// The expiry of a cookie whose Expires attribute is 'date', capped to the longest lifetime. The capping happens on
// seconds, as dates far from now do not fit in a SysTimePoint of nanoseconds.
SysTimePoint ExpiryOfDate(std::chrono::sys_seconds date, SysTimePoint now) noexcept {
  const auto nowSeconds = std::chrono::floor<std::chrono::seconds>(now);
  if (date <= nowSeconds) {
    return kExpired;
  }
  return date - nowSeconds >= kMaxLifetime ? now + kMaxLifetime : SysTimePoint{date.time_since_epoch()};
}

// Parses the value of a Max-Age attribute ("-"? DIGIT+) into an expiry. Returns false when it is not one.
constexpr bool ParseMaxAge(std::string_view value, SysTimePoint now, SysTimePoint& expiry) noexcept {
  const bool negative = value.starts_with('-');
  const std::string_view digits = value.substr(negative ? 1U : 0U);
  if (digits.empty() || !std::ranges::all_of(digits, isdigit)) {
    return false;
  }
  // The number is not computed past the longest lifetime, so that no number of digits overflows.
  int64_t seconds = 0;
  for (const char digit : digits) {
    seconds = std::min((seconds * 10) + (digit - '0'), static_cast<int64_t>(kMaxLifetime.count()));
  }
  expiry = negative || seconds == 0 ? kExpired : now + std::chrono::seconds{seconds};
  return true;
}

// A Set-Cookie header value split into its name, value and attributes (RFC 6265bis §5.6).
struct ParsedSetCookie {
  std::string_view name;
  std::string_view value;
  std::string_view domain;  // without leading dot, any case, empty when absent: a host-only cookie
  std::string_view path;    // empty when absent or not starting with '/'
  SysTimePoint expiry{kSessionExpiry};
  bool secure{false};
  bool httpOnly{false};
  CookieSameSite sameSite{CookieSameSite::Default};
};

// The SameSite attribute of 'value' (case-insensitive), Default for an unknown one.
constexpr CookieSameSite ParseSameSite(std::string_view value) noexcept {
  char buf[6];  // "strict", the longest value
  const std::string_view lowerValue = LowerCaseInto(value, buf);
  if (lowerValue == "strict") {
    return CookieSameSite::Strict;
  }
  if (lowerValue == "lax") {
    return CookieSameSite::Lax;
  }
  return lowerValue == "none" ? CookieSameSite::None : CookieSameSite::Default;
}

// The cookie of 'cookies' with these identifying attributes, end() if none. Out of line to keep this loop, linear in
// the number of cookies of a site, in registers: in the large store(), GCC can leave its iterator on the stack.
template <class Cookies>
AERONET_NOINLINE auto FindSameCookie(Cookies& cookies, bool hostOnly, std::string_view name, std::string_view domain,
                                     std::string_view path) noexcept {
  return std::ranges::find_if(cookies, [=](const auto& cookie) {
    return cookie.hostOnly == hostOnly && cookie.name() == name && cookie.domain() == domain && cookie.path() == path;
  });
}

// Parses 'setCookieValue'. Returns false when the whole header is ignored.
bool ParseSetCookie(std::string_view setCookieValue, SysTimePoint now, ParsedSetCookie& out) noexcept {
  // Only the bytes of a received field value are allowed: a control character other than a tab invalidates the whole
  // header.
  if (!http::IsValidReceivedHeaderValue(setCookieValue)) {
    return false;
  }
  const std::size_t nameValueEnd = setCookieValue.find(';');
  const std::string_view nameValue = setCookieValue.substr(0, nameValueEnd);
  if (const std::size_t equal = nameValue.find('='); equal == std::string_view::npos) {
    out.value = TrimOws(nameValue);  // a nameless cookie
  } else {
    out.name = TrimOws(nameValue.substr(0, equal));
    out.value = TrimOws(nameValue.substr(equal + 1));
  }
  if (out.name.size() + out.value.size() > kMaxNameValueSize || (out.name.empty() && out.value.empty())) {
    return false;
  }

  // The last valid occurrence of an attribute wins, Max-Age over Expires whatever their order.
  bool hasMaxAge = false;
  std::string_view attributes =
      nameValueEnd == std::string_view::npos ? std::string_view{} : setCookieValue.substr(nameValueEnd + 1);
  while (!attributes.empty()) {
    const std::size_t attributeEnd = attributes.find(';');
    const std::string_view attribute = attributes.substr(0, attributeEnd);
    attributes = attributeEnd == std::string_view::npos ? std::string_view{} : attributes.substr(attributeEnd + 1);

    const std::size_t equal = attribute.find('=');
    const std::string_view value =
        equal == std::string_view::npos ? std::string_view{} : TrimOws(attribute.substr(equal + 1));
    if (value.size() > kMaxAttributeValueSize) {
      continue;
    }
    char nameBuf[kMaxAttributeNameSize];
    const std::string_view name = LowerCaseInto(TrimOws(attribute.substr(0, equal)), nameBuf);
    if (name == "expires") {
      std::chrono::sys_seconds date;
      if (!hasMaxAge && ParseCookieDate(value, date)) {
        out.expiry = ExpiryOfDate(date, now);
      }
    } else if (name == "max-age") {
      hasMaxAge = ParseMaxAge(value, now, out.expiry) || hasMaxAge;
    } else if (name == "domain") {
      // An empty value is ignored. A leading dot is legacy syntax, so "." alone is an empty domain, which makes the
      // cookie host-only (RFC 6265bis §5.6.3, §5.7).
      if (!value.empty()) {
        out.domain = value.substr(value.starts_with('.') ? 1U : 0U);
      }
    } else if (name == "path") {
      out.path = value.starts_with('/') ? value : std::string_view{};
    } else if (name == "secure") {
      out.secure = true;
    } else if (name == "httponly") {
      out.httpOnly = true;
    } else if (name == "samesite") {
      out.sameSite = ParseSameSite(value);
    }
  }
  return true;
}

// Removes the cookies for which 'pred' is true, which is called once for each cookie, in order, and keeps the order of
// the others. Returns the number of cookies removed.
template <class Cookies, class Pred>
Cookies::size_type EraseCookiesIf(Cookies& cookies, Pred pred) {
  const typename Cookies::size_type sizeBefore = cookies.size();
  cookies.erase(std::ranges::remove_if(cookies, std::move(pred)).begin(), cookies.end());
  return sizeBefore - cookies.size();
}

// Sets 'data' to the name, value, domain and path of a cookie (see CookieJar::StoredCookie), in a single allocation.
void AssignCookieData(RawChars32& data, std::string_view name, std::string_view value, std::string_view domain,
                      std::string_view path) {
  data.clear();
  data.ensureAvailableCapacity(name.size() + value.size() + domain.size() + path.size());
  char* pData = Append(name, data.data());
  pData = Append(value, pData);
  pData = Append(domain, pData);
  pData = Append(path, pData);
  data.setEnd(pData);
}

// The URL of the public API, as the cookie rules see it. Throws std::invalid_argument for an invalid URL.
internal::CookieRequestUrl ParseRequestUrl(std::string_view url) {
  const internal::UrlParseResult res = internal::ParseUrl(url);
  if (res.host.empty()) {
    throw std::invalid_argument("Invalid http or https URL for the cookie jar");
  }
  return {res.host, TargetPath(res.target), res.isTls};
}

internal::CookieRequestUrl RequestUrlOf(const HttpRequest& request) noexcept {
  return {request.host(), TargetPath(request.target()), request.isTlsRequest()};
}

}  // namespace

CookieJar::CookieJar(uint32_t maxCookies, uint32_t maxCookiesPerDomain) noexcept
    : _maxCookies(maxCookies), _maxCookiesPerDomain(maxCookiesPerDomain) {
  assert(maxCookies > 0 && maxCookiesPerDomain > 0);
}

CookieJar::CookieJar(CookieJar&& rhs) noexcept
    : _cookiesByDomain(std::move(rhs._cookiesByDomain)),
      _requestCookies(std::move(rhs._requestCookies)),
      _sequence(rhs._sequence),
      _nbCookies(std::exchange(rhs._nbCookies, 0)),
      _maxCookies(rhs._maxCookies),
      _maxCookiesPerDomain(rhs._maxCookiesPerDomain) {}

CookieJar& CookieJar::operator=(CookieJar&& rhs) noexcept {
  if (this != &rhs) [[likely]] {
    _cookiesByDomain = std::move(rhs._cookiesByDomain);
    _requestCookies = std::move(rhs._requestCookies);
    _sequence = rhs._sequence;
    _nbCookies = std::exchange(rhs._nbCookies, 0);
    _maxCookies = rhs._maxCookies;
    _maxCookiesPerDomain = rhs._maxCookiesPerDomain;
  }
  return *this;
}

bool CookieJar::setCookie(std::string_view url, std::string_view setCookieValue, SysTimePoint now) {
  return store(ParseRequestUrl(url), setCookieValue, now);
}

std::string CookieJar::cookieHeader(std::string_view url, SysTimePoint now) const {
  RawChars cookies;
  AppendCookies(*this, ParseRequestUrl(url), internal::CookieSiteContext::SameSite, now, cookies);
  return std::string(std::string_view(cookies));
}

bool CookieJar::store(internal::CookieRequestUrl url, std::string_view setCookieValue, SysTimePoint now) {
  ParsedSetCookie parsed;
  if (!ParseSetCookie(setCookieValue, now, parsed)) {
    return false;
  }
  char hostBuf[kMaxHostSize];
  const std::string_view host = LowerCaseInto(url.host, hostBuf);
  if (host.empty()) {
    return false;
  }
  const bool secureUrl = url.https || IsLoopbackHost(host);

  // Domain (RFC 6265bis §5.7 steps 5 to 7): a public suffix is only accepted as the host itself, then host-only.
  bool hostOnly = true;
  std::string_view domain = host;
  char domainBuf[kMaxHostSize];
  if (!parsed.domain.empty()) {
    // Empty when longer than a host name: refused below, like a public suffix other than 'host'.
    const std::string_view domainAttribute = LowerCaseInto(parsed.domain, domainBuf);
    if (RegistrableDomain(domainAttribute).empty()) {
      if (domainAttribute != host) {
        return false;
      }
    } else if (!DomainMatch(host, domainAttribute)) {
      return false;
    } else {
      hostOnly = false;
      domain = domainAttribute;
    }
  }

  // Secure cookies only come from secure URLs, and so do the names reserved to them, also when spelled as the value of
  // a nameless cookie (RFC 6265bis §4.1.3, §5.7 steps 14, 20 and 21).
  if (parsed.secure && !secureUrl) {
    return false;
  }
  const std::string_view prefixedName = parsed.name.empty() ? parsed.value : parsed.name;
  char prefixBuf[kSecureNamePrefix.size()];
  const std::string_view namePrefix = LowerCaseInto(prefixedName.substr(0, sizeof(prefixBuf)), prefixBuf);
  if (namePrefix.starts_with(kSecureNamePrefix) && (parsed.name.empty() || !parsed.secure)) {
    return false;
  }
  const std::string_view path = parsed.path.empty() ? DefaultPath(url.path) : parsed.path;
  if (namePrefix.starts_with(kHostNamePrefix) && (parsed.name.empty() || !parsed.secure || !hostOnly || path != "/")) {
    return false;
  }

  // The lengths fit the 16-bit fields of StoredCookie: ParseSetCookie bounds the name and the value, and 'domain' was
  // lower-cased into a buffer of kMaxHostSize.
  static_assert(kMaxNameValueSize <= std::numeric_limits<decltype(StoredCookie::valueLen)>::max() &&
                kMaxHostSize <= std::numeric_limits<decltype(StoredCookie::domainLen)>::max());
  assert(parsed.name.size() + parsed.value.size() <= kMaxNameValueSize && domain.size() <= kMaxHostSize);

  const std::string_view domainKey = DomainKey(host);
  auto domainIt = _cookiesByDomain.find(domainKey);
  if (domainIt != _cookiesByDomain.end()) {
    Cookies& cookies = domainIt->second;
    // An http URL cannot shadow a Secure cookie of the same name (RFC 6265bis §5.7 step 16).
    if (!parsed.secure && !secureUrl && std::ranges::any_of(cookies, [&](const StoredCookie& cookie) {
          return cookie.secure && cookie.name() == parsed.name &&
                 (DomainMatch(domain, cookie.domain()) || DomainMatch(cookie.domain(), domain)) &&
                 PathMatch(path, cookie.path());
        })) {
      return false;
    }
    const auto sameCookieIt = FindSameCookie(cookies, hostOnly, parsed.name, domain, path);
    if (sameCookieIt != cookies.end()) {
      if (parsed.expiry <= now) {
        cookies.erase(sameCookieIt);
        --_nbCookies;
        if (cookies.empty()) {
          _cookiesByDomain.erase(domainIt);
        }
        return true;
      }
      // Same path, same creation: the replacement keeps the position of the cookie in the sorted list.
      StoredCookie& cookie = *sameCookieIt;
      AssignCookieData(cookie.data, parsed.name, parsed.value, domain, path);
      cookie.valueLen = static_cast<uint16_t>(parsed.value.size());
      cookie.expiry = parsed.expiry;
      cookie.lastAccess = ++_sequence;
      cookie.secure = parsed.secure;
      cookie.httpOnly = parsed.httpOnly;
      cookie.sameSite = parsed.sameSite;
      return true;
    }
  }
  if (parsed.expiry <= now) {
    return true;  // deletes a cookie that does not exist
  }

  if (domainIt == _cookiesByDomain.end()) {
    domainIt = _cookiesByDomain.try_emplace(domainKey).first;
  }
  Cookies& cookies = domainIt->second;
  RawChars32 data;
  AssignCookieData(data, parsed.name, parsed.value, domain, path);
  // The newest cookie goes after the ones whose path is not shorter.
  const auto insertIt = std::ranges::partition_point(
      cookies, [pathSize = path.size()](const StoredCookie& cookie) { return cookie.path().size() >= pathSize; });
  cookies.emplace(insertIt, StoredCookie{
                                .data = std::move(data),
                                .expiry = parsed.expiry,
                                .lastAccess = ++_sequence,
                                .nameLen = static_cast<uint16_t>(parsed.name.size()),
                                .valueLen = static_cast<uint16_t>(parsed.value.size()),
                                .domainLen = static_cast<uint16_t>(domain.size()),
                                .hostOnly = hostOnly,
                                .secure = parsed.secure,
                                .httpOnly = parsed.httpOnly,
                                .sameSite = parsed.sameSite,
                            });
  ++_nbCookies;
  if (cookies.size() > _maxCookiesPerDomain) {
    evict(&cookies, _maxCookiesPerDomain, now);
  }
  if (_nbCookies > _maxCookies) {
    evict(nullptr, _maxCookies, now);
  }
  return true;
}

template <class Jar>
void CookieJar::AppendCookies(Jar& jar, internal::CookieRequestUrl url, internal::CookieSiteContext context,
                              SysTimePoint now, RawChars& out) {
  static constexpr bool kMarksSent = !std::is_const_v<Jar>;
  if (jar._nbCookies == 0) {
    return;
  }
  char hostBuf[kMaxHostSize];
  const std::string_view host = LowerCaseInto(url.host, hostBuf);
  if (host.empty()) {
    return;
  }
  const auto domainIt = jar._cookiesByDomain.find(DomainKey(host));
  if (domainIt == jar._cookiesByDomain.end()) {
    return;
  }
  const bool secureUrl = url.https || IsLoopbackHost(host);
  const std::size_t initialSize = out.size();
  [[maybe_unused]] uint64_t access = 0;
  if constexpr (kMarksSent) {
    access = ++jar._sequence;
  }
  // The cookies are already in the order of the header: longest paths first, then oldest first. 'cookie' is const
  // when 'jar' is.
  for (auto& cookie : domainIt->second) {
    if (cookie.expiry <= now || (cookie.secure && !secureUrl) || !SameSiteAllows(cookie.sameSite, context) ||
        !(cookie.hostOnly ? host == cookie.domain() : DomainMatch(host, cookie.domain())) ||
        !PathMatch(url.path, cookie.path())) {
      continue;
    }

    const std::string_view name = cookie.name();
    const std::string_view value = cookie.value();

    out.ensureAvailableCapacityExponential(http::CookiePairSep.size() + name.size() + 1U + value.size());

    char* pData = out.end();
    if (out.size() != initialSize) {
      pData = Append(http::CookiePairSep, pData);
    }
    if (!name.empty()) {
      pData = Append(name, pData);
      *pData++ = '=';
    }
    pData = Append(value, pData);
    out.setEnd(pData);
    if constexpr (kMarksSent) {
      cookie.lastAccess = access;
    }
  }
}

void CookieJar::storeResponseCookies(const HttpRequest& request, const HttpResponse& response, SysTimePoint now) {
  const internal::CookieRequestUrl url = RequestUrlOf(request);
  for (const auto& [name, value] : response.headers()) {
    if (name == http::SetCookie) {
      // A refused cookie is not an error of the exchange: the browsers ignore it silently too.
      static_cast<void>(store(url, value, now));
    }
  }
}

void CookieJar::prepareRequestCookies(const HttpRequest& request, internal::CookieSiteContext context,
                                      SysTimePoint now) {
  _requestCookies.clear();
  AppendCookies(*this, RequestUrlOf(request), context, now, _requestCookies);
}

void CookieJar::prepareCacheKeyCookies(const HttpRequest& request, SysTimePoint now) {
  _requestCookies.clear();
  // The first exchange of a request has no redirect before it: the same-site context.
  AppendCookies(std::as_const(*this), RequestUrlOf(request), internal::CookieSiteContext::SameSite, now,
                _requestCookies);
}

void CookieJar::evict(Cookies* pCookies, std::size_t maxCount, SysTimePoint now) {
  if (pCookies != nullptr) {
    _nbCookies -= EraseCookiesIf(*pCookies, [now](const StoredCookie& cookie) { return cookie.expiry <= now; });
  } else {
    eraseExpired(now);
  }
  const uint32_t count = pCookies != nullptr ? pCookies->size() : _nbCookies;
  if (count <= maxCount) {
    return;
  }

  // The least recently used cookies are those whose last access is below the one of rank 'nbToEvict'.
  const std::size_t nbToEvict = count - (maxCount - (maxCount / 10U));
  // Allocated per call: evicting a tenth below the limit leaves 'maxCount / 10' insertions before the next call, and a
  // member buffer would keep the memory of the largest eviction.
  vector<uint64_t> lastAccesses;
  lastAccesses.reserve(count);
  const auto collect = [&lastAccesses](const Cookies& cookies) {
    for (const StoredCookie& cookie : cookies) {
      lastAccesses.push_back(cookie.lastAccess);
    }
  };
  if (pCookies != nullptr) {
    collect(*pCookies);
  } else {
    for (const auto& [domainKey, cookies] : _cookiesByDomain) {
      collect(cookies);
    }
  }
  const auto thresholdIt = lastAccesses.begin() + static_cast<std::ptrdiff_t>(nbToEvict - 1U);
  std::ranges::nth_element(lastAccesses, thresholdIt);
  const uint64_t threshold = *thresholdIt;
  // Cookies sent by the same request share their last access: those at the threshold go in their order until enough.
  std::size_t nbAtThreshold =
      nbToEvict - static_cast<std::size_t>(
                      std::ranges::count_if(lastAccesses, [threshold](uint64_t access) { return access < threshold; }));
  const auto evictFrom = [this, threshold, &nbAtThreshold](Cookies& cookies) {
    _nbCookies -= EraseCookiesIf(cookies, [threshold, &nbAtThreshold](const StoredCookie& cookie) {
      if (cookie.lastAccess < threshold) {
        return true;
      }
      if (cookie.lastAccess == threshold && nbAtThreshold != 0) {
        --nbAtThreshold;
        return true;
      }
      return false;
    });
  };
  if (pCookies != nullptr) {
    evictFrom(*pCookies);
    return;
  }
  for (auto domainIt = _cookiesByDomain.begin(); domainIt != _cookiesByDomain.end();) {
    evictFrom(domainIt->second);
    domainIt = domainIt->second.empty() ? _cookiesByDomain.erase(domainIt) : std::next(domainIt);
  }
}

void CookieJar::eraseExpired(SysTimePoint now) {
  for (auto domainIt = _cookiesByDomain.begin(); domainIt != _cookiesByDomain.end();) {
    Cookies& cookies = domainIt->second;
    _nbCookies -= EraseCookiesIf(cookies, [now](const StoredCookie& cookie) { return cookie.expiry <= now; });
    domainIt = cookies.empty() ? _cookiesByDomain.erase(domainIt) : std::next(domainIt);
  }
}

void CookieJar::removeExpired(SysTimePoint now) {
  eraseExpired(now);
  for (auto& [domainKey, cookies] : _cookiesByDomain) {
    cookies.shrink_to_fit();
  }
  _cookiesByDomain.shrink_to_fit();
  _requestCookies.shrink_to_fit();
}

void CookieJar::clear() noexcept {
  _cookiesByDomain.clear();
  _nbCookies = 0;
}

}  // namespace aeronet
