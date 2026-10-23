#include "aeronet/https-redirect.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/raw-chars.hpp"

using namespace aeronet;

namespace {
// Builds "https://host[:port]path[?query...]" the way SingleHttpServer::emitHttpsRedirect does,
// so the unit test exercises the same composition the server relies on.
std::string BuildUrl(std::string_view host, uint16_t port, std::string_view path,
                     std::initializer_list<std::pair<std::string_view, std::string_view>> query = {}) {
  RawChars buf;
  if (!http::AppendHttpsAuthority(buf, host, port)) {
    return {};
  }
  http::AppendUrlEncodedPath(buf, path);
  char sep = '?';
  for (const auto& [key, value] : query) {
    http::AppendUrlEncodedQueryParam(buf, sep, key, value);
    sep = '&';
  }
  return std::string(std::string_view(buf));
}
}  // namespace

TEST(HttpsRedirectTest, StripPortPlainHost) {
  EXPECT_EQ(http::StripPortFromHost("example.com"), "example.com");
  EXPECT_EQ(http::StripPortFromHost("example.com:80"), "example.com");
  EXPECT_EQ(http::StripPortFromHost("example.com:8080"), "example.com");
}

TEST(HttpsRedirectTest, StripPortIPv4) {
  EXPECT_EQ(http::StripPortFromHost("192.168.0.1"), "192.168.0.1");
  EXPECT_EQ(http::StripPortFromHost("192.168.0.1:80"), "192.168.0.1");
}

TEST(HttpsRedirectTest, StripPortIPv6Bracketed) {
  EXPECT_EQ(http::StripPortFromHost("[::1]"), "[::1]");
  EXPECT_EQ(http::StripPortFromHost("[::1]:80"), "[::1]");
  EXPECT_EQ(http::StripPortFromHost("[2001:db8::1]:8443"), "[2001:db8::1]");
}

TEST(HttpsRedirectTest, StripPortIPv6MissingClosingBracket) {
  // No closing ']' -> malformed, returned unchanged (best-effort).
  EXPECT_EQ(http::StripPortFromHost("[::1"), "[::1");
  EXPECT_EQ(http::StripPortFromHost("["), "[");
}

TEST(HttpsRedirectTest, StripPortEmpty) { EXPECT_TRUE(http::StripPortFromHost("").empty()); }

TEST(HttpsRedirectTest, StandardPortOmitted) {
  EXPECT_EQ(BuildUrl("example.com", 443, "/path"), "https://example.com/path");
}

TEST(HttpsRedirectTest, NonStandardPortAppended) {
  EXPECT_EQ(BuildUrl("example.com", 8443, "/path"), "https://example.com:8443/path");
}

TEST(HttpsRedirectTest, HostPortReplacedByTargetPort) {
  // Incoming Host carries the plaintext port (80) which must be dropped and replaced by the HTTPS port.
  EXPECT_EQ(BuildUrl("example.com:80", 443, "/"), "https://example.com/");
  EXPECT_EQ(BuildUrl("example.com:80", 8443, "/"), "https://example.com:8443/");
}

TEST(HttpsRedirectTest, QueryPreserved) {
  EXPECT_EQ(BuildUrl("example.com", 443, "/search", {{"q", "foo"}, {"lang", "en"}}),
            "https://example.com/search?q=foo&lang=en");
}

TEST(HttpsRedirectTest, EmptyQueryNoQuestionMark) {
  EXPECT_EQ(BuildUrl("example.com", 443, "/x"), "https://example.com/x");
}

TEST(HttpsRedirectTest, IPv6Url) { EXPECT_EQ(BuildUrl("[::1]:80", 8443, "/a"), "https://[::1]:8443/a"); }

TEST(HttpsRedirectTest, EmptyHostReturnsFalse) {
  RawChars buf;
  buf.append("garbage");
  EXPECT_FALSE(http::AppendHttpsAuthority(buf, "", 443));
  EXPECT_TRUE(std::string_view(buf).empty());  // cleared even on failure
}

TEST(HttpsRedirectTest, HostThatIsOnlyAPortReturnsFalse) {
  RawChars buf;
  // ":80" strips to an empty host -> cannot build absolute URL.
  EXPECT_FALSE(http::AppendHttpsAuthority(buf, ":80", 443));
}

TEST(HttpsRedirectTest, PathWithUnsafeCharactersIsEncoded) {
  // Decoded space and '?' inside the path must be percent-encoded so the URL stays valid.
  EXPECT_EQ(BuildUrl("example.com", 443, "/a b"), "https://example.com/a%20b");
  EXPECT_EQ(BuildUrl("example.com", 443, "/a?b"), "https://example.com/a%3Fb");
  // A literal '%' (from a decoded "%25") is re-encoded back to "%25".
  EXPECT_EQ(BuildUrl("example.com", 443, "/100%"), "https://example.com/100%25");
  // Path slashes and sub-delims are preserved verbatim.
  EXPECT_EQ(BuildUrl("example.com", 443, "/a/b+c"), "https://example.com/a/b+c");
}

TEST(HttpsRedirectTest, QueryComponentsAreEncoded) {
  // Structural characters inside a decoded value must be encoded so they cannot break out of the component.
  EXPECT_EQ(BuildUrl("example.com", 443, "/s", {{"q", "a&b=c"}}), "https://example.com/s?q=a%26b%3Dc");
  EXPECT_EQ(BuildUrl("example.com", 443, "/s", {{"name", "John Doe"}}), "https://example.com/s?name=John%20Doe");
}

TEST(HttpsRedirectTest, PathSafeCharactersPreservedVerbatim) {
  // Every RFC 3986 path-safe character (unreserved + sub-delims + ':' '@' '/') is kept verbatim,
  // exercising all branches of the path-encoding predicate.
  static constexpr std::string_view kPath = "/a:b@c!d$e&f'g(h)i*j+k,l;m=n_o~p-q.r/s";
  EXPECT_EQ(BuildUrl("example.com", 443, kPath), std::string("https://example.com").append(kPath));
}

TEST(HttpsRedirectTest, QueryUnreservedCharactersPreserved) {
  // Unreserved characters ('-', '.', '_', '~') in keys and values are kept verbatim.
  EXPECT_EQ(BuildUrl("example.com", 443, "/s", {{"a_b~c", "d-e.f~g_h"}}), "https://example.com/s?a_b~c=d-e.f~g_h");
}

TEST(HttpsRedirectTest, QueryEmptyValueKeepsEquals) {
  EXPECT_EQ(BuildUrl("example.com", 443, "/s", {{"flag", ""}}), "https://example.com/s?flag=");
}

TEST(HttpsRedirectTest, MultipleQueryParamsJoinedWithAmpersand) {
  EXPECT_EQ(BuildUrl("example.com", 443, "/s", {{"a", "1"}, {"b", "2"}, {"c", "3"}}),
            "https://example.com/s?a=1&b=2&c=3");
}
