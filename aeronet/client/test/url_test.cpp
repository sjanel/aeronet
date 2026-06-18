#include "aeronet/url.hpp"

#include <gtest/gtest.h>

#include <string_view>

#include "aeronet/http-client-error.hpp"

namespace aeronet {
namespace {

// Assert that Url::Parse rejects `url` with HttpClientErrc::invalidUrl (no exception thrown).
void ExpectInvalidUrl(std::string_view url) {
  const auto parsed = Url::Parse(url);
  ASSERT_FALSE(parsed.has_value()) << url;
  EXPECT_EQ(parsed.error(), HttpClientErrc::invalidUrl) << url;
}

}  // namespace

TEST(UrlTest, ParsesBasicHttp) {
  const Url url = Url::Parse("http://example.com/path?q=1").value();
  EXPECT_FALSE(url.tls());
  EXPECT_EQ(url.scheme(), "http");
  EXPECT_EQ(url.host(), "example.com");
  EXPECT_EQ(url.port(), 80);
  EXPECT_EQ(url.target(), "/path?q=1");
  EXPECT_TRUE(url.isDefaultPort());
}

TEST(UrlTest, ParsesHttpsDefaultPort) {
  const Url url = Url::Parse("https://example.com").value();
  EXPECT_TRUE(url.tls());
  EXPECT_EQ(url.scheme(), "https");
  EXPECT_EQ(url.port(), 443);
  EXPECT_EQ(url.target(), "/");  // empty path normalises to "/"
  EXPECT_TRUE(url.isDefaultPort());
}

TEST(UrlTest, ParsesExplicitPort) {
  const Url url = Url::Parse("http://localhost:8080/").value();
  EXPECT_EQ(url.port(), 8080);
  EXPECT_FALSE(url.isDefaultPort());
}

TEST(UrlTest, HttpsExplicitDefaultPortIsDefault) {
  const Url url = Url::Parse("https://host:443/x").value();
  EXPECT_TRUE(url.isDefaultPort());
}

TEST(UrlTest, HostIsNullTerminated) {
  const Url url = Url::Parse("http://example.com:9000/path").value();
  EXPECT_STREQ(url.hostCStr().c_str(), "example.com");
  // The ':' separator is restored once the guard is destroyed.
  EXPECT_EQ(url.originKey(), "http://example.com:9000");
  EXPECT_EQ(url.target(), "/path");
}

TEST(UrlTest, OriginKeyExplicitPort) {
  const Url url = Url::Parse("http://example.com:8080/p?q=1").value();
  EXPECT_EQ(url.originKey(), "http://example.com:8080");
  EXPECT_EQ(url.target(), "/p?q=1");
}

TEST(UrlTest, OriginKeyDefaultPort) {
  const Url url = Url::Parse("https://example.com/").value();
  EXPECT_EQ(url.originKey(), "https://example.com:443");
  EXPECT_EQ(url.target(), "/");
}

TEST(UrlTest, OriginKeyIPv6Unbracketed) {
  const Url url = Url::Parse("http://[::1]:9000/").value();
  EXPECT_EQ(url.originKey(), "http://::1:9000");
  EXPECT_EQ(url.host(), "::1");
}

TEST(UrlTest, ParsesIPv6Literal) {
  const Url url = Url::Parse("http://[::1]:9000/path").value();
  EXPECT_EQ(url.host(), "::1");
  EXPECT_EQ(url.port(), 9000);
  EXPECT_EQ(url.target(), "/path");
}

TEST(UrlTest, StripsFragment) {
  const Url url = Url::Parse("http://h/p?a=b#frag").value();
  EXPECT_EQ(url.target(), "/p?a=b");
}

TEST(UrlTest, RejectsUnknownScheme) {
  ExpectInvalidUrl("ftp://example.com");
  ExpectInvalidUrl("example.com");
  ExpectInvalidUrl("://x");
}

TEST(UrlTest, SchemeIsCaseInsensitive) {
  const Url url = Url::Parse("HTTPS://example.com/").value();
  EXPECT_TRUE(url.tls());
}

TEST(UrlTest, RejectsBadPort) {
  ExpectInvalidUrl("http://h:0/");
  ExpectInvalidUrl("http://h:70000/");
  ExpectInvalidUrl("http://h:abc/");
}

TEST(UrlTest, RejectsUserinfo) { ExpectInvalidUrl("http://user:pass@host/"); }

TEST(UrlTest, ResolveAbsoluteRedirect) {
  const Url base = Url::Parse("http://a.com/x").value();
  const Url redir = base.resolveRedirect("https://b.com/y").value();
  EXPECT_TRUE(redir.tls());
  EXPECT_EQ(redir.host(), "b.com");
  EXPECT_EQ(redir.target(), "/y");
}

TEST(UrlTest, ResolveRootRelativeRedirect) {
  const Url base = Url::Parse("http://a.com/x/y?z=1").value();
  const Url redir = base.resolveRedirect("/new").value();
  EXPECT_EQ(redir.host(), "a.com");
  EXPECT_EQ(redir.target(), "/new");
}

TEST(UrlTest, ResolveRelativePathRedirect) {
  const Url base = Url::Parse("http://a.com/dir/page").value();
  const Url redir = base.resolveRedirect("other").value();
  EXPECT_EQ(redir.target(), "/dir/other");
}

TEST(UrlTest, ResolveNetworkPathRedirect) {
  const Url base = Url::Parse("https://a.com/x").value();
  const Url redir = base.resolveRedirect("//c.com/z").value();
  EXPECT_TRUE(redir.tls());  // inherits scheme
  EXPECT_EQ(redir.host(), "c.com");
  EXPECT_EQ(redir.target(), "/z");
}

TEST(UrlTest, ResolveNetworkPathRedirectRejectsMalformedAuthority) {
  // A network-path reference (//authority) whose authority is malformed (empty host before the port)
  // cannot be resolved: the redirect surfaces HttpClientErrc::invalidUrl.
  const Url base = Url::Parse("http://a.com/x").value();
  const auto redir = base.resolveRedirect("//:99/bad");
  ASSERT_FALSE(redir.has_value());
  EXPECT_EQ(redir.error(), HttpClientErrc::invalidUrl);
}

TEST(UrlTest, RejectsSameLengthWrongScheme) {
  // Same length as "http" but different characters: exercises the per-char scheme compare mismatch.
  ExpectInvalidUrl("htxp://example.com/");
  ExpectInvalidUrl("xxxxx://example.com/");  // same length as "https"
}

TEST(UrlTest, RejectsMalformedIPv6) {
  ExpectInvalidUrl("http://[::1/");    // missing closing bracket
  ExpectInvalidUrl("http://[::1]x/");  // junk after ']' (not a ':')
}

TEST(UrlTest, AcceptsBracketedIPv6WithoutPort) {
  const Url url = Url::Parse("http://[::1]/p").value();
  EXPECT_EQ(url.host(), "::1");
  EXPECT_EQ(url.port(), 80);
  EXPECT_EQ(url.target(), "/p");
}

TEST(UrlTest, RejectsEmptyHost) {
  ExpectInvalidUrl("http://:8080/");  // empty host before the port
}

TEST(UrlTest, RejectsEmptyAuthority) {
  // "http:///path" has an empty authority (the path starts immediately after "://").
  ExpectInvalidUrl("http:///path");
}

TEST(UrlTest, RejectsPortWithTrailingJunk) {
  // Valid leading digits followed by junk: the port does not consume the whole token.
  ExpectInvalidUrl("http://h:80x/");
}

TEST(UrlTest, ResolveEmptyRedirectReturnsError) {
  const Url base = Url::Parse("http://a.com/x").value();
  const auto redir = base.resolveRedirect("");
  ASSERT_FALSE(redir.has_value());
  EXPECT_EQ(redir.error(), HttpClientErrc::invalidUrl);
}

TEST(UrlTest, ResolveRelativeRedirectStripsFragmentAndBaseQuery) {
  const Url base = Url::Parse("http://a.com/dir/page?keep=0").value();
  // Fragment in the relative ref is dropped, and the base query is not part of the directory.
  const Url redir = base.resolveRedirect("other?x=1#frag").value();
  EXPECT_EQ(redir.target(), "/dir/other?x=1");
}

TEST(UrlTest, ResolveRelativeRedirectFromQueryOnlyBase) {
  // A base whose target is query-only ("?q") has no directory component, so a relative reference resolves
  // against the root.
  const Url base = Url::Parse("http://a.com?q=1").value();
  const Url redir = base.resolveRedirect("rel").value();
  EXPECT_EQ(redir.host(), "a.com");
  EXPECT_EQ(redir.target(), "/rel");
}

}  // namespace aeronet
