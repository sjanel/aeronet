#include "aeronet/cookie-jar.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/http-client.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {
namespace {

// 2026-11-07 12:00:00 UTC.
constexpr SysTimePoint kNow = std::chrono::sys_days{std::chrono::year{2026} / 11 / 7} + std::chrono::hours{12};

}  // namespace

// Friend of CookieJar, for the cookies of the requests of HttpClient.
class CookieJarTest : public ::testing::Test {
 protected:
  bool set(std::string_view url, std::string_view setCookieValue, SysTimePoint now = kNow) {
    return jar.setCookie(url, setCookieValue, now);
  }

  std::string header(std::string_view url, SysTimePoint now = kNow) { return jar.cookieHeader(url, now); }

  // The cookies that HttpClient sends with a GET request to 'url', which marks them as sent.
  std::string send(CookieJar& cookieJar, std::string_view url) {
    cookieJar.prepareRequestCookies(client.makeRequest(http::Method::GET, url), internal::CookieSiteContext::SameSite,
                                    kNow);
    return std::string(cookieJar.requestCookies());
  }

  // The cookies that HttpClient puts in the cache key of a GET request to 'url', which leaves them as they are.
  std::string cacheKeyCookies(CookieJar& cookieJar, std::string_view url) {
    cookieJar.prepareCacheKeyCookies(client.makeRequest(http::Method::GET, url), kNow);
    return std::string(cookieJar.requestCookies());
  }

  // The names of the cookies of 'cookieJar', sorted.
  static vector<std::string> Names(const CookieJar& cookieJar) {
    vector<std::string> names;
    cookieJar.forEach([&names](const Cookie& cookie) { names.emplace_back(cookie.name); });
    std::ranges::sort(names);
    return names;
  }

  // The only cookie of the jar.
  [[nodiscard]] Cookie single() const {
    EXPECT_EQ(jar.size(), 1U);
    Cookie found{};
    jar.forEach([&found](const Cookie& cookie) { found = cookie; });
    return found;
  }

  HttpClient client;  // makes the requests of send() and cacheKeyCookies()
  CookieJar jar;
};

namespace {

TEST_F(CookieJarTest, StoresAndSendsCookies) {
  EXPECT_TRUE(jar.empty());
  EXPECT_TRUE(set("http://example.com/", "session=abc"));
  EXPECT_TRUE(set("http://example.com/", "theme=dark"));
  EXPECT_EQ(jar.size(), 2U);
  EXPECT_EQ(header("http://example.com/"), "session=abc; theme=dark");
  EXPECT_EQ(header("http://example.com/any/path?query=1"), "session=abc; theme=dark");
  EXPECT_EQ(header("http://other.com/"), "");
  EXPECT_EQ(header("http://www.example.com/"), "");  // host-only

  const std::string_view url = "http://example.com/";
  EXPECT_TRUE(set(url, "session=def"));  // replaced, keeps its place
  EXPECT_EQ(header(url), "session=def; theme=dark");
  EXPECT_EQ(jar.size(), 2U);
}

TEST_F(CookieJarTest, NameAndValueSyntax) {
  const std::string_view url = "http://example.com/";
  EXPECT_TRUE(set(url, "  spaced\t=\t value with space  ; Path=/"));
  EXPECT_TRUE(set(url, "nameless"));
  EXPECT_TRUE(set(url, "empty="));
  EXPECT_TRUE(set(url, "=onlyvalue"));  // empty name, like a nameless cookie
  EXPECT_TRUE(set(url, R"(quoted="a b")"));
  EXPECT_TRUE(set(url, "equals=a=b"));
  EXPECT_EQ(header(url), R"(spaced=value with space; onlyvalue; empty=; quoted="a b"; equals=a=b)");
  // "nameless" and "=onlyvalue" are both the nameless cookie: the second replaced the first.
  EXPECT_EQ(jar.size(), 5U);

  EXPECT_FALSE(set(url, ""));
  EXPECT_FALSE(set(url, "="));
  EXPECT_FALSE(set(url, " ; Path=/"));
  EXPECT_FALSE(set(url, "ctl=a\x01"));
  EXPECT_FALSE(set(url, "ctl=a\x7F"));
  EXPECT_FALSE(set(url, "ctl=a; Path=/\n"));
  EXPECT_TRUE(set(url, "tab=a\tb"));

  // 4096 bytes of name and value at most.
  EXPECT_TRUE(set(url, "big=" + std::string(4093, 'x')));
  EXPECT_FALSE(set(url, "big=" + std::string(4094, 'x')));
  EXPECT_EQ(jar.size(), 7U);
}

TEST_F(CookieJarTest, AttributesAreCaseInsensitiveAndTheLastOneWins) {
  EXPECT_TRUE(set("https://example.com/a/b", "id=1; PATH=/a; path=/; SECURE; HttpOnly; SameSite=lax; Unknown=x; Flag"));
  const Cookie cookie = single();
  EXPECT_EQ(cookie.name, "id");
  EXPECT_EQ(cookie.value, "1");
  EXPECT_EQ(cookie.domain, "example.com");
  EXPECT_EQ(cookie.path, "/");
  EXPECT_TRUE(cookie.hostOnly);
  EXPECT_TRUE(cookie.secure);
  EXPECT_TRUE(cookie.httpOnly);
  EXPECT_EQ(cookie.sameSite, CookieSameSite::Lax);
  EXPECT_FALSE(cookie.persistent());
  EXPECT_EQ(cookie.expiry, SysTimePoint::max());
}

TEST_F(CookieJarTest, SameSiteValues) {
  const std::string_view url = "http://example.com/";
  const auto sameSiteOf = [this, url](std::string_view setCookieValue) {
    jar.clear();
    EXPECT_TRUE(set(url, setCookieValue));
    return single().sameSite;
  };
  EXPECT_EQ(sameSiteOf("a=1; SameSite=Strict"), CookieSameSite::Strict);
  EXPECT_EQ(sameSiteOf("a=1; samesite=LAX"), CookieSameSite::Lax);
  EXPECT_EQ(sameSiteOf("a=1; SameSite=None"), CookieSameSite::None);
  EXPECT_EQ(sameSiteOf("a=1; SameSite=Strict; SameSite=bogus"), CookieSameSite::Default);
  EXPECT_EQ(sameSiteOf("a=1; SameSite=Strict; SameSite=strictly"), CookieSameSite::Default);
  EXPECT_EQ(sameSiteOf("a=1; SameSite=Lax; SameSite="), CookieSameSite::Default);
  EXPECT_EQ(sameSiteOf("a=1"), CookieSameSite::Default);
}

TEST_F(CookieJarTest, UnknownAttributesAreIgnored) {
  // Names longer than the known ones, or starting like them, are unknown.
  EXPECT_TRUE(
      set("http://example.com/dir/page", "a=1; HttpOnlyX; SameSiteX=Strict; Pathname=/; Secure2; =x; Expire=2"));
  const Cookie cookie = single();
  EXPECT_FALSE(cookie.httpOnly);
  EXPECT_EQ(cookie.sameSite, CookieSameSite::Default);
  EXPECT_EQ(cookie.path, "/dir");
  EXPECT_FALSE(cookie.secure);
  EXPECT_FALSE(cookie.persistent());
}

TEST_F(CookieJarTest, AttributeValuesLongerThan1024BytesAreIgnored) {
  EXPECT_TRUE(set("http://example.com/dir/page", "a=1; Path=/" + std::string(1024, 'p')));
  EXPECT_EQ(single().path, "/dir");  // default path
  jar.clear();
  EXPECT_TRUE(set("http://example.com/dir/page", "a=1; Path=/" + std::string(1023, 'p')));
  EXPECT_EQ(single().path.size(), 1024U);
}

TEST_F(CookieJarTest, DomainAttribute) {
  EXPECT_TRUE(set("http://www.example.com/", "a=1; Domain=example.com"));
  EXPECT_TRUE(set("http://www.example.com/", "b=2; Domain=.EXAMPLE.com"));
  EXPECT_TRUE(set("http://www.example.com/", "c=3; Domain=www.example.com"));
  EXPECT_TRUE(set("http://www.example.com/", "d=4; Domain="));  // ignored attribute: host-only
  EXPECT_EQ(header("http://example.com/"), "a=1; b=2");
  EXPECT_EQ(header("http://deep.sub.example.com/"), "a=1; b=2");
  EXPECT_EQ(header("http://www.example.com/"), "a=1; b=2; c=3; d=4");
  EXPECT_EQ(header("http://sub.www.example.com/"), "a=1; b=2; c=3");
  EXPECT_EQ(header("http://notexample.com/"), "");

  // A domain that does not match the host, or a public suffix, is refused.
  EXPECT_FALSE(set("http://www.example.com/", "e=5; Domain=other.com"));
  EXPECT_FALSE(set("http://www.example.com/", "e=5; Domain=sub.www.example.com"));
  EXPECT_FALSE(set("http://www.example.com/", "e=5; Domain=ample.com"));
  EXPECT_FALSE(set("http://www.example.com/", "e=5; Domain=com"));
  EXPECT_FALSE(set("http://www.example.co.uk/", "e=5; Domain=co.uk"));
  EXPECT_FALSE(set("http://www.example.ne.jp/", "e=5; Domain=ne.jp"));
  EXPECT_FALSE(set("http://www.example.co.uk/", "e=5; Domain=.uk"));
  // Longer than a host name (but not than an attribute value): it matches no host.
  EXPECT_FALSE(set("http://www.example.com/", "e=5; Domain=" + std::string(250, 'a') + ".example.com"));
  EXPECT_EQ(jar.size(), 4U);

  // A country-code top-level domain whose second level is a name: two labels.
  EXPECT_TRUE(set("http://www.example.fr/", "h=8; Domain=example.fr"));
  EXPECT_EQ(header("http://shop.example.fr/"), "h=8");
  // The registrable domain of a country-code second-level domain has three labels.
  EXPECT_TRUE(set("http://www.example.co.uk/", "f=6; Domain=example.co.uk"));
  EXPECT_EQ(header("http://shop.example.co.uk/"), "f=6");
  EXPECT_EQ(header("http://other.co.uk/"), "");
  // Two labels whose top-level domain is not a country code are a registrable domain.
  EXPECT_TRUE(set("http://www.co.com/", "g=7; Domain=co.com"));
  EXPECT_EQ(header("http://co.com/"), "g=7");
}

TEST_F(CookieJarTest, DotDomainMakesAHostOnlyCookie) {
  // "Domain=." is an empty domain once its leading dot is removed: the cookie is host-only, as without the attribute.
  EXPECT_TRUE(set("http://example.com/", "a=1; Domain=."));
  EXPECT_TRUE(single().hostOnly);
  EXPECT_EQ(single().domain, "example.com");
  // As the last Domain attribute, it also cancels a previous one, unlike an empty one, which is ignored.
  EXPECT_TRUE(set("http://www.example.com/", "b=2; Domain=example.com; Domain=."));
  EXPECT_TRUE(set("http://www.example.com/", "c=3; Domain=example.com; Domain="));
  EXPECT_EQ(header("http://example.com/"), "a=1; c=3");
  EXPECT_EQ(header("http://www.example.com/"), "b=2; c=3");
  EXPECT_EQ(header("http://sub.www.example.com/"), "c=3");
}

TEST_F(CookieJarTest, PublicSuffixHostsOnlyHaveHostOnlyCookies) {
  // A Domain equal to a host that is a public suffix makes a host-only cookie.
  EXPECT_TRUE(set("http://localhost:8080/", "a=1; Domain=localhost"));
  EXPECT_TRUE(single().hostOnly);
  EXPECT_TRUE(set("http://co.uk/", "b=2; Domain=co.uk"));
  EXPECT_TRUE(set("http://localhost/", "c=3"));
  EXPECT_EQ(header("http://localhost/"), "a=1; c=3");
  EXPECT_EQ(header("http://co.uk/"), "b=2");
  EXPECT_EQ(header("http://www.co.uk/"), "");
  EXPECT_EQ(header("http://sub.localhost/"), "");
  EXPECT_FALSE(set("http://sub.localhost/", "d=4; Domain=localhost"));
  EXPECT_FALSE(set("http://.com/", "e=5; Domain=.com"));
  // Hosts starting or ending with a dot, malformed, keep their cookies to themselves.
  for (const std::string_view url : {"http://.com/", "http://.co.uk/", "http://example.com./"}) {
    jar.clear();
    EXPECT_TRUE(set(url, "f=6")) << url;
    EXPECT_EQ(header(url), "f=6") << url;
    EXPECT_EQ(header("http://example.com/"), "") << url;
  }
}

TEST_F(CookieJarTest, IpAddressHosts) {
  EXPECT_TRUE(set("http://192.168.1.10/", "a=1"));
  EXPECT_TRUE(set("http://192.168.1.10/", "b=2; Domain=192.168.1.10"));
  EXPECT_FALSE(set("http://192.168.1.10/", "c=3; Domain=1.10"));
  EXPECT_FALSE(set("http://192.168.1.10/", "c=3; Domain=168.1.10"));
  EXPECT_EQ(header("http://192.168.1.10:8080/"), "a=1; b=2");
  EXPECT_EQ(header("http://10.192.168.1.10/"), "");  // not an IPv4 address: another host
  EXPECT_EQ(header("http://168.1.10/"), "");

  EXPECT_TRUE(set("http://[2001:db8::1]/", "v6=1"));
  EXPECT_EQ(header("http://[2001:db8::1]:8080/x"), "v6=1");
  EXPECT_EQ(header("http://[2001:db8::2]/"), "");
}

TEST_F(CookieJarTest, HostsAreCaseInsensitive) {
  EXPECT_TRUE(set("http://WWW.Example.COM/", "a=1; Domain=EXAMPLE.com"));
  EXPECT_TRUE(set("http://WWW.Example.COM/", "b=2"));
  EXPECT_EQ(header("http://www.example.com/"), "a=1; b=2");
  EXPECT_EQ(header("http://Www.EXAMPLE.com/"), "a=1; b=2");
  jar.forEach(
      [](const Cookie& cookie) { EXPECT_EQ(cookie.domain, cookie.name == "a" ? "example.com" : "www.example.com"); });
}

TEST_F(CookieJarTest, HostsLongerThanAHostNameGetNoCookies) {
  const std::string longHost(256, 'a');
  EXPECT_FALSE(set("http://" + longHost + ".com/", "a=1"));
  EXPECT_TRUE(set("http://" + std::string(251, 'a') + ".com/", "a=1"));
  EXPECT_EQ(header("http://" + std::string(251, 'a') + ".com/"), "a=1");
  EXPECT_EQ(header("http://" + longHost + ".com/"), "");
  EXPECT_EQ(send(jar, "http://" + longHost + ".com/"), "");
}

TEST_F(CookieJarTest, DefaultPathIsTheDirectoryOfTheRequestPath) {
  const auto pathOf = [this](std::string_view url) {
    jar.clear();
    EXPECT_TRUE(set(url, "a=1"));
    return std::string(single().path);
  };
  EXPECT_EQ(pathOf("http://example.com"), "/");
  EXPECT_EQ(pathOf("http://example.com/"), "/");
  EXPECT_EQ(pathOf("http://example.com/page"), "/");
  EXPECT_EQ(pathOf("http://example.com/dir/"), "/dir");
  EXPECT_EQ(pathOf("http://example.com/dir/sub/page?x=/a/b"), "/dir/sub");
  EXPECT_EQ(pathOf("http://example.com?x=/a/b"), "/");
  // A Path attribute that does not start with a slash is replaced by the default path.
  jar.clear();
  EXPECT_TRUE(set("http://example.com/dir/page", "a=1; Path=relative"));
  EXPECT_EQ(single().path, "/dir");
  jar.clear();
  EXPECT_TRUE(set("http://example.com/dir/page", "a=1; Path=/x; Path="));
  EXPECT_EQ(single().path, "/dir");
}

TEST_F(CookieJarTest, PathMatching) {
  EXPECT_TRUE(set("http://example.com/", "a=1; Path=/docs"));
  EXPECT_TRUE(set("http://example.com/", "b=2; Path=/docs/"));
  EXPECT_EQ(header("http://example.com/docs"), "a=1");
  EXPECT_EQ(header("http://example.com/docs/"), "b=2; a=1");
  EXPECT_EQ(header("http://example.com/docs/web/page"), "b=2; a=1");
  EXPECT_EQ(header("http://example.com/docsets"), "");
  EXPECT_EQ(header("http://example.com/"), "");
  EXPECT_EQ(header("http://example.com/Docs"), "");
}

TEST_F(CookieJarTest, UrlsWithoutPathHaveTheRootPath) {
  EXPECT_TRUE(set("http://example.com/", "a=1; Path=/"));
  EXPECT_TRUE(set("http://example.com/", "b=2; Path=/dir"));
  EXPECT_EQ(header("http://example.com"), "a=1");
  EXPECT_EQ(header("http://example.com?x=1"), "a=1");
  EXPECT_EQ(header("http://example.com:8080?x=/dir/page"), "a=1");  // the slashes of the query are not of a path
  EXPECT_EQ(header("http://example.com/?x=1"), "a=1");
  EXPECT_EQ(send(jar, "http://example.com?x=/dir/page"), "a=1");
}

TEST_F(CookieJarTest, LongestPathsFirstThenOldestFirst) {
  const std::string_view url = "http://example.com/a/b/c";
  EXPECT_TRUE(set(url, "root1=1; Path=/"));
  EXPECT_TRUE(set(url, "deep=1; Path=/a/b"));
  EXPECT_TRUE(set(url, "root2=1; Path=/"));
  EXPECT_TRUE(set(url, "mid=1; Path=/a"));
  EXPECT_TRUE(set(url, "deep2=1; Path=/a/b"));
  EXPECT_EQ(header(url), "deep=1; deep2=1; mid=1; root1=1; root2=1");
  // A replaced cookie keeps its creation time, so its place; a deleted then recreated one is the newest.
  EXPECT_TRUE(set(url, "deep=2; Path=/a/b"));
  EXPECT_TRUE(set(url, "root1=1; Path=/; Max-Age=0"));
  EXPECT_TRUE(set(url, "root1=3; Path=/"));
  EXPECT_EQ(header(url), "deep=2; deep2=1; mid=1; root2=1; root1=3");
}

TEST_F(CookieJarTest, SameNameWithAnotherPathOrDomainIsAnotherCookie) {
  EXPECT_TRUE(set("http://www.example.com/x/y", "a=root; Path=/"));
  EXPECT_TRUE(set("http://www.example.com/x/y", "a=x; Path=/x"));
  EXPECT_TRUE(set("http://www.example.com/x/y", "a=domain; Path=/; Domain=example.com"));
  EXPECT_TRUE(set("http://www.example.com/x/y", "a=host; Path=/; Domain=www.example.com"));
  EXPECT_EQ(jar.size(), 4U);
  EXPECT_EQ(header("http://www.example.com/x/y"), "a=x; a=root; a=domain; a=host");
  // Deletion matches the domain and the host-only flag.
  EXPECT_TRUE(set("http://www.example.com/", "a=; Max-Age=0; Domain=www.example.com"));
  EXPECT_EQ(header("http://www.example.com/x/y"), "a=x; a=root; a=domain");
}

TEST_F(CookieJarTest, MaxAge) {
  const std::string_view url = "http://example.com/";
  EXPECT_TRUE(set(url, "a=1; Max-Age=60"));
  EXPECT_TRUE(single().persistent());
  EXPECT_EQ(single().expiry, kNow + std::chrono::seconds{60});
  EXPECT_EQ(header(url, kNow + std::chrono::seconds{59}), "a=1");
  EXPECT_EQ(header(url, kNow + std::chrono::seconds{60}), "");

  // Invalid values are ignored: the cookie stays a session one.
  for (const std::string_view invalid : {
           "a=1; Max-Age=",
           "a=1; Max-Age=-",
           "a=1; Max-Age=1.5",
           "a=1; Max-Age=+5",
           "a=1; Max-Age=5s",
           "a=1; Max-Age= - 5",
       }) {
    jar.clear();
    EXPECT_TRUE(set(url, invalid));
    EXPECT_FALSE(single().persistent()) << invalid;
  }

  // Capped to 400 days, whatever the number of digits.
  jar.clear();
  EXPECT_TRUE(set(url, "a=1; Max-Age=99999999999999999999999999999"));
  EXPECT_EQ(single().expiry, kNow + std::chrono::days{400});

  // Zero or negative deletes.
  EXPECT_TRUE(set(url, "a=1; Max-Age=0"));
  EXPECT_TRUE(jar.empty());
  EXPECT_TRUE(set(url, "a=1"));
  EXPECT_TRUE(set(url, "a=1; Max-Age=-1"));
  EXPECT_TRUE(jar.empty());
  // Deleting a cookie that does not exist is accepted, and stores nothing.
  EXPECT_TRUE(set(url, "b=1; Max-Age=0"));
  EXPECT_TRUE(jar.empty());
}

TEST_F(CookieJarTest, MaxAgeWinsOverExpiresInAnyOrder) {
  const std::string_view url = "http://example.com/";
  EXPECT_TRUE(set(url, "a=1; Max-Age=10; Expires=Wed, 09 Jun 2027 10:18:14 GMT"));
  EXPECT_EQ(single().expiry, kNow + std::chrono::seconds{10});
  jar.clear();
  EXPECT_TRUE(set(url, "a=1; Expires=Wed, 09 Jun 2027 10:18:14 GMT; Max-Age=10"));
  EXPECT_EQ(single().expiry, kNow + std::chrono::seconds{10});
  // An invalid Max-Age after a valid one leaves the valid one.
  jar.clear();
  EXPECT_TRUE(set(url, "a=1; Max-Age=10; Max-Age=x"));
  EXPECT_EQ(single().expiry, kNow + std::chrono::seconds{10});
  // An invalid Max-Age leaves Expires.
  jar.clear();
  EXPECT_TRUE(set(url, "a=1; Max-Age=x; Expires=Wed, 09 Jun 2027 10:18:14 GMT"));
  EXPECT_EQ(single().expiry, std::chrono::sys_days{std::chrono::year{2027} / 6 / 9} + std::chrono::hours{10} +
                                 std::chrono::minutes{18} + std::chrono::seconds{14});
}

TEST_F(CookieJarTest, ExpiresDateFormats) {
  const std::string_view url = "http://example.com/";
  const SysTimePoint expected = std::chrono::sys_days{std::chrono::year{2027} / 1 / 2} + std::chrono::hours{3} +
                                std::chrono::minutes{4} + std::chrono::seconds{5};
  for (const std::string_view date : {
           "Sat, 02 Jan 2027 03:04:05 GMT",     // RFC 1123
           "Saturday, 02-Jan-27 03:04:05 GMT",  // RFC 850
           "Sat Jan  2 03:04:05 2027",          // asctime
           "Sat, 02-Jan-2027 03:04:05 GMT",     // Netscape
           "2 january 2027 3:4:5",              // lenient: long month name, 1-digit fields
           "Sat, 02 Jan 2027 03:04:05Z",        // a non-digit ends the seconds
           "  02 JAN 2027 03:04:05  ",          // case, surrounding delimiters
           "03:04:05 02 jan 27",                // any order
           "02\tat Jan 2027 03:04:05",          // tab, short tokens skipped
           "[02]_Jan_{2027}~03:04:05",          // delimiters of each range
           "@02=Jan<2027>03:04:05?",
           "02 Jan 2027 03:04:05 ,",
           "02 Jan 2027 03:04:05 h\xC3\xA9",  // bytes above 0x7E are not delimiters  // trailing delimiter
       }) {
    jar.clear();
    EXPECT_TRUE(set(url, "a=1; Expires=" + std::string(date)));
    EXPECT_EQ(single().expiry, expected) << date;
  }

  // Invalid dates are ignored: the cookie stays a session one.
  for (const std::string_view date : {
           "",
           "tomorrow",
           "Sat, 02 Jan 2027",       // no time
           "02 Jan 2027 24:00:00",   // hour out of range
           "02 Jan 2027 23:60:00",   // minute out of range
           "02 Jan 2027 23:00:60",   // second out of range
           "30 Feb 2027 03:04:05",   // no such day
           "32 Jan 2027 03:04:05",   // day out of range
           "00 Jan 2027 03:04:05",   // day out of range
           "02 Foo 2027 03:04:05",   // no month
           "02 Jan 1600 03:04:05",   // before 1601
           "02 Jan 03:04:05",        // no year
           "02 Jan 20270 03:04:05",  // year of 5 digits
           "123 Jan 2027 03:04:05",  // 3 digits make a year, not a day: no day
           "02 Jan 2027 3a:04:05",   // hours with a non-digit
           "02 Jan 2027 03:4a:05",   // minutes with a non-digit
           "02 Jan 2027 03:04:",     // empty seconds
           "02 Jan 2027 003:04:05",  // hours of 3 digits
       }) {
    jar.clear();
    EXPECT_TRUE(set(url, "a=1; Expires=" + std::string(date)));
    EXPECT_FALSE(single().persistent()) << date;
  }
}

TEST_F(CookieJarTest, ExpiresYearsAndLimits) {
  const std::string_view url = "http://example.com/";
  const auto expiryOf = [this, url](std::string_view date) {
    jar.clear();
    EXPECT_TRUE(set(url, "a=1; Expires=" + std::string(date)));
    return jar.empty() ? SysTimePoint::min() : single().expiry;
  };
  // Two-digit years: 70 to 99 are 19xx (in the past: deleted), 0 to 69 are 20xx.
  EXPECT_EQ(expiryOf("01 Jan 99 00:00:00"), SysTimePoint::min());
  EXPECT_EQ(expiryOf("01 Jan 27 00:00:00"), std::chrono::sys_days{std::chrono::year{2027} / 1 / 1});
  // Capped to 400 days, also for dates that a SysTimePoint cannot represent.
  EXPECT_EQ(expiryOf("01 Jan 2030 00:00:00"), kNow + std::chrono::days{400});
  EXPECT_EQ(expiryOf("31 Dec 9999 23:59:59"), kNow + std::chrono::days{400});
  // In the past, even far: the cookie is deleted.
  EXPECT_EQ(expiryOf("01 Jan 1601 00:00:00"), SysTimePoint::min());
  EXPECT_TRUE(set(url, "a=1"));
  EXPECT_TRUE(set(url, "a=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT"));
  EXPECT_TRUE(jar.empty());
}

TEST_F(CookieJarTest, SecureCookies) {
  EXPECT_FALSE(set("http://example.com/", "a=1; Secure"));
  EXPECT_TRUE(set("https://example.com/", "a=1; Secure"));
  EXPECT_TRUE(set("https://example.com/", "b=2"));
  EXPECT_EQ(header("https://example.com/"), "a=1; b=2");
  EXPECT_EQ(header("http://example.com/"), "b=2");

  // Loopback hosts are secure contexts, as in browsers.
  for (const std::string_view url : {
           "http://localhost:8080/",
           "http://app.localhost/",
           "http://127.0.0.1:1234/",
           "http://127.1.2.3/",
           "http://[::1]:80/",
       }) {
    jar.clear();
    EXPECT_TRUE(set(url, "a=1; Secure")) << url;
    EXPECT_EQ(header(url), "a=1") << url;
  }
  jar.clear();
  EXPECT_FALSE(set("http://127.example.com/", "a=1; Secure"));
  EXPECT_FALSE(set("http://notlocalhost/", "a=1; Secure"));
}

TEST_F(CookieJarTest, HttpUrlsCannotShadowSecureCookies) {
  EXPECT_TRUE(set("https://www.example.com/", "session=secure; Secure; Domain=example.com"));
  // Same name, overlapping domain and path, from http: refused.
  EXPECT_FALSE(set("http://www.example.com/", "session=evil"));
  EXPECT_FALSE(set("http://www.example.com/", "session=evil; Domain=example.com"));
  EXPECT_FALSE(set("http://example.com/", "session=evil"));
  EXPECT_FALSE(set("http://www.example.com/sub/page", "session=evil; Path=/sub"));
  // A sibling host is not concerned.
  EXPECT_TRUE(set("https://a.example.com/", "sibling=secure; Secure"));
  EXPECT_TRUE(set("http://b.example.com/", "sibling=fine"));
  // A Domain covering the host of a host-only secure cookie.
  EXPECT_TRUE(set("https://www.example.com/", "hostonly=secure; Secure"));
  EXPECT_FALSE(set("http://www.example.com/", "hostonly=evil; Domain=example.com"));
  // Another name, a path that the secure cookie does not cover, or https: accepted.
  EXPECT_TRUE(set("http://www.example.com/", "other=1"));
  EXPECT_TRUE(set("https://www.example.com/", "session=fine"));
  EXPECT_EQ(jar.size(), 6U);

  jar.clear();
  EXPECT_TRUE(set("https://example.com/admin/x", "session=secure; Secure; Path=/admin"));
  EXPECT_TRUE(set("http://example.com/", "session=root; Path=/"));
  EXPECT_FALSE(set("http://example.com/", "session=evil; Path=/admin/deeper"));
  // A cookie of another registrable domain is not concerned.
  EXPECT_TRUE(set("http://other.com/", "session=1"));
}

TEST_F(CookieJarTest, CookieNamePrefixes) {
  // __Secure-: Secure, from a secure URL.
  EXPECT_TRUE(set("https://www.example.com/", "__Secure-a=1; Secure; Domain=example.com"));
  EXPECT_FALSE(set("https://www.example.com/", "__Secure-b=1"));
  EXPECT_FALSE(set("http://www.example.com/", "__Secure-c=1; Secure"));
  EXPECT_FALSE(set("https://www.example.com/", "__SECURE-d=1"));  // case-insensitive
  // __Host-: Secure, host-only, Path=/.
  EXPECT_TRUE(set("https://www.example.com/", "__Host-e=1; Secure; Path=/"));
  EXPECT_TRUE(set("https://www.example.com/", "__Host-f=1; Secure"));  // the default path of "/" is "/"
  EXPECT_FALSE(set("https://www.example.com/", "__Host-g=1; Secure; Path=/; Domain=www.example.com"));
  EXPECT_FALSE(set("https://www.example.com/dir/page", "__Host-h=1; Secure"));  // default path "/dir"
  EXPECT_FALSE(set("https://www.example.com/", "__Host-i=1; Secure; Path=/dir"));
  EXPECT_FALSE(set("https://www.example.com/", "__host-j=1; Path=/"));  // case-insensitive, not Secure
  // A Domain equal to a host that is a public suffix still makes a host-only cookie.
  EXPECT_TRUE(set("https://localhost/", "__Host-k=1; Secure; Path=/; Domain=localhost"));
  // A nameless cookie cannot look like a prefixed one.
  EXPECT_FALSE(set("https://www.example.com/", "__Secure-l; Secure"));
  EXPECT_FALSE(set("https://www.example.com/", "=__Host-m; Secure; Path=/"));
  EXPECT_EQ(jar.size(), 4U);
  // Names shorter than a prefix, or close to one, are not prefixed.
  EXPECT_TRUE(set("http://www.example.com/dir/page", "__Host=1"));
  EXPECT_TRUE(set("http://www.example.com/", "__Secure=1"));
  EXPECT_TRUE(set("http://www.example.com/", "_Secure-n=1"));
  EXPECT_EQ(jar.size(), 7U);
}

TEST_F(CookieJarTest, EvictsLeastRecentlyUsedCookiesOfADomain) {
  const std::string_view url = "http://example.com/";
  CookieJar smallJar(100, 10);
  for (int idx = 0; idx < 10; ++idx) {
    EXPECT_TRUE(smallJar.setCookie(url, "c" + std::to_string(idx) + "=1", kNow));
  }
  // A replacement is a use: c2 and c3 become the least recently used.
  EXPECT_TRUE(smallJar.setCookie(url, "c0=1", kNow));
  EXPECT_TRUE(smallJar.setCookie(url, "c1=2", kNow));
  EXPECT_EQ(smallJar.size(), 10U);
  // The 11th cookie evicts down to 9 (10 minus a tenth): the 2 least recently used.
  EXPECT_TRUE(smallJar.setCookie(url, "new=1", kNow));
  EXPECT_EQ(smallJar.size(), 9U);
  EXPECT_EQ(smallJar.cookieHeader(url, kNow), "c0=1; c1=2; c4=1; c5=1; c6=1; c7=1; c8=1; c9=1; new=1");
  // Another domain is not concerned.
  EXPECT_TRUE(smallJar.setCookie("http://other.com/", "o=1", kNow));
  EXPECT_EQ(smallJar.size(), 10U);
}

TEST_F(CookieJarTest, EvictsExpiredCookiesFirst) {
  const std::string_view url = "http://example.com/";
  CookieJar smallJar(100, 4);
  EXPECT_TRUE(smallJar.setCookie(url, "a=1", kNow));
  EXPECT_TRUE(smallJar.setCookie(url, "short=1; Max-Age=1", kNow));
  EXPECT_TRUE(smallJar.setCookie(url, "b=1", kNow));
  EXPECT_TRUE(smallJar.setCookie(url, "c=1", kNow));
  const SysTimePoint later = kNow + std::chrono::seconds{2};
  EXPECT_TRUE(smallJar.setCookie(url, "d=1", later));
  EXPECT_EQ(smallJar.size(), 4U);
  EXPECT_EQ(smallJar.cookieHeader(url, later), "a=1; b=1; c=1; d=1");
}

TEST_F(CookieJarTest, EvictsLeastRecentlyUsedCookiesOfAllDomains) {
  CookieJar smallJar(20, 10);
  const auto siteUrl = [](int idx) { return "http://site" + std::to_string(idx) + ".com/"; };
  for (int idx = 0; idx < 20; ++idx) {
    EXPECT_TRUE(smallJar.setCookie(siteUrl(idx), "c=1", kNow));
  }
  EXPECT_EQ(send(smallJar, siteUrl(0)), "c=1");  // site0 is used again
  // The 21st cookie evicts down to 18 (20 minus a tenth): site1, site2 and site3, whose domains go away.
  EXPECT_TRUE(smallJar.setCookie(siteUrl(20), "c=1", kNow));
  EXPECT_EQ(smallJar.size(), 18U);
  for (int idx = 0; idx <= 20; ++idx) {
    EXPECT_EQ(smallJar.cookieHeader(siteUrl(idx), kNow), idx >= 1 && idx <= 3 ? "" : "c=1") << idx;
  }
}

TEST_F(CookieJarTest, EvictionAmongCookiesSentTogether) {
  const std::string_view url = "http://example.com/";
  CookieJar smallJar(100, 5);
  for (int idx = 0; idx < 5; ++idx) {
    EXPECT_TRUE(smallJar.setCookie(url, "c" + std::to_string(idx) + "=1", kNow));
  }
  // The five cookies are sent together, so share their last use: the first ones of the jar go first.
  EXPECT_EQ(send(smallJar, url), "c0=1; c1=1; c2=1; c3=1; c4=1");
  EXPECT_TRUE(smallJar.setCookie(url, "new=1", kNow));
  EXPECT_EQ(smallJar.cookieHeader(url, kNow), "c1=1; c2=1; c3=1; c4=1; new=1");
}

TEST_F(CookieJarTest, OnlySentCookiesAreUsed) {
  CookieJar smallJar(100, 3);
  EXPECT_TRUE(smallJar.setCookie("http://example.com/", "a=1; Path=/a", kNow));
  EXPECT_TRUE(smallJar.setCookie("http://example.com/", "b=1; Path=/b", kNow));
  EXPECT_TRUE(smallJar.setCookie("http://example.com/", "c=1; Path=/c", kNow));
  // Neither cookieHeader() nor the cookies of a cache key are a use: 'a' stays the least recently used.
  EXPECT_EQ(smallJar.cookieHeader("http://example.com/a", kNow), "a=1");
  EXPECT_EQ(cacheKeyCookies(smallJar, "http://example.com/a"), "a=1");
  EXPECT_TRUE(smallJar.setCookie("http://example.com/", "d=1; Path=/d", kNow));
  EXPECT_EQ(Names(smallJar), (vector<std::string>{"b", "c", "d"}));
  // Sending 'b' makes 'c' the least recently used.
  EXPECT_EQ(send(smallJar, "http://example.com/b"), "b=1");
  EXPECT_TRUE(smallJar.setCookie("http://example.com/", "e=1; Path=/e", kNow));
  EXPECT_EQ(Names(smallJar), (vector<std::string>{"b", "d", "e"}));
}

TEST_F(CookieJarTest, MovedFromJarIsEmptyAndReusable) {
  const std::string_view url = "http://example.com/";
  CookieJar source(4, 4);
  EXPECT_TRUE(source.setCookie(url, "a=1", kNow));
  EXPECT_TRUE(source.setCookie(url, "b=1", kNow));
  CookieJar moved(std::move(source));
  EXPECT_EQ(moved.size(), 2U);
  EXPECT_EQ(moved.cookieHeader(url, kNow), "a=1; b=1");
  // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(source.empty());
  EXPECT_EQ(source.size(), 0U);
  EXPECT_EQ(source.cookieHeader(url, kNow), "");
  // Reused with its limits, its count stays the one of its cookies.
  for (int idx = 0; idx < 6; ++idx) {
    EXPECT_TRUE(source.setCookie(url, "c" + std::to_string(idx) + "=1", kNow));
  }
  EXPECT_EQ(source.size(), 4U);
  EXPECT_EQ(Names(source).size(), 4U);

  moved = std::move(source);
  EXPECT_EQ(moved.size(), 4U);
  EXPECT_EQ(Names(moved), (vector<std::string>{"c2", "c3", "c4", "c5"}));
  EXPECT_TRUE(source.empty());
  EXPECT_TRUE(Names(source).empty());
  // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)

  // A copy has the same cookies, and the copied jar keeps them.
  CookieJar copy(moved);
  EXPECT_EQ(copy.size(), 4U);
  EXPECT_EQ(copy.cookieHeader(url, kNow), moved.cookieHeader(url, kNow));
  copy.clear();
  copy = moved;
  EXPECT_EQ(Names(copy), Names(moved));
}

TEST_F(CookieJarTest, RemoveExpiredAndClear) {
  EXPECT_TRUE(set("http://example.com/", "a=1; Max-Age=10"));
  EXPECT_TRUE(set("http://example.com/", "b=1"));
  EXPECT_TRUE(set("http://other.com/", "c=1; Max-Age=10"));
  EXPECT_EQ(jar.size(), 3U);
  // Expired cookies are not sent, and counted until removed.
  EXPECT_EQ(header("http://example.com/", kNow + std::chrono::seconds{10}), "b=1");
  EXPECT_EQ(jar.size(), 3U);
  jar.removeExpired(kNow + std::chrono::seconds{10});
  EXPECT_EQ(jar.size(), 1U);
  EXPECT_EQ(single().name, "b");
  jar.clear();
  EXPECT_TRUE(jar.empty());
  EXPECT_EQ(header("http://example.com/"), "");
}

TEST_F(CookieJarTest, InvalidUrlsThrow) {
  EXPECT_THROW(set("ftp://example.com/", "a=1"), std::invalid_argument);
  EXPECT_THROW(set("example.com", "a=1"), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(header("http:///path")), std::invalid_argument);
  EXPECT_TRUE(jar.empty());
}

TEST_F(CookieJarTest, ForEachListsEveryCookie) {
  EXPECT_TRUE(set("https://www.example.com/dir/page", "a=1; Domain=example.com; Max-Age=60; Secure; SameSite=Strict"));
  EXPECT_TRUE(set("http://other.com/", "b=2; HttpOnly"));
  vector<std::string> listed;
  jar.forEach([&listed](const Cookie& cookie) {
    listed.push_back(std::string(cookie.name) + '=' + std::string(cookie.value) + ' ' + std::string(cookie.domain) +
                     std::string(cookie.path) + (cookie.hostOnly ? " hostOnly" : "") +
                     (cookie.secure ? " secure" : "") + (cookie.httpOnly ? " httpOnly" : "") +
                     (cookie.persistent() ? " persistent" : ""));
  });
  std::ranges::sort(listed);
  ASSERT_EQ(listed.size(), 2U);
  EXPECT_EQ(listed[0], "a=1 example.com/dir secure persistent");
  EXPECT_EQ(listed[1], "b=2 other.com/ hostOnly httpOnly");
}

}  // namespace
}  // namespace aeronet
