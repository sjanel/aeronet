// Unit coverage for ClientRequest: the fluent (lvalue & rvalue) builder API and the getters. No sockets.
#include "aeronet/client-request.hpp"

#include <gtest/gtest.h>

#include <string>

#include "../src/client-request-builder.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/url.hpp"

namespace aeronet {

TEST(ClientRequestBuilderTest, AuthorityLenWithDefaultPort) {
  auto url = Url::Parse("http://example.com:80/path");
  ASSERT_TRUE(url.has_value());

  EXPECT_EQ(internal::AuthorityLen(*url, false), 11U);  // "example.com"
  EXPECT_EQ(internal::AuthorityLen(*url, true), 13U);   // "[example.com]"
}

TEST(ClientRequestBuilderTest, AuthorityLenWithNonDefaultPort) {
  auto url = Url::Parse("http://example.com:8080/path");
  ASSERT_TRUE(url.has_value());

  EXPECT_EQ(internal::AuthorityLen(*url, false), 16U);  // "example.com:8080"
  EXPECT_EQ(internal::AuthorityLen(*url, true), 18U);   // "[example.com:8080]"
}

TEST(ClientRequestBuilderTest, AppendAuthorityWithDefaultPort) {
  auto url = Url::Parse("http://example.com:80/path");
  ASSERT_TRUE(url.has_value());

  char buf[32];
  std::string_view out(buf, internal::AppendAuthority(buf, *url, false));
  EXPECT_EQ(out, "example.com");

  out = std::string_view(buf, internal::AppendAuthority(buf, *url, true));
  EXPECT_EQ(out, "[example.com]");
}

TEST(ClientRequestBuilderTest, AppendAuthorityWithNonDefaultPort) {
  auto url = Url::Parse("http://example.com:8080/path");
  ASSERT_TRUE(url.has_value());

  char buf[32];
  std::string_view out(buf, internal::AppendAuthority(buf, *url, false));
  EXPECT_EQ(out, "example.com:8080");

  out = std::string_view(buf, internal::AppendAuthority(buf, *url, true));
  EXPECT_EQ(out, "[example.com]:8080");
}

TEST(ClientRequestTest, DefaultConstructed) {
  ClientRequest req;
  EXPECT_EQ(req.method(), http::Method::GET);
  EXPECT_TRUE(req.url().empty());
  EXPECT_TRUE(req.body().empty());
}

TEST(ClientRequestTest, MethodAndUrlConstructor) {
  ClientRequest req(http::Method::POST, "http://example.com/x");
  EXPECT_EQ(req.method(), http::Method::POST);
  EXPECT_EQ(req.url(), "http://example.com/x");
}

TEST(ClientRequestTest, LvalueFluentSetters) {
  ClientRequest req;
  req.method(http::Method::PUT).url("http://h/p").header("X-A", "1").headerAddLine("X-B", "2").body("payload");
  EXPECT_EQ(req.method(), http::Method::PUT);
  EXPECT_EQ(req.url(), "http://h/p");
  EXPECT_EQ(req.body(), "payload");
}

TEST(ClientRequestTest, BodyWithContentType) {
  ClientRequest req(http::Method::POST, "http://h/p");
  req.body("data", "application/json");
  EXPECT_EQ(req.body(), "data");
}

TEST(ClientRequestTest, VeryLongUrlIsNotTruncated) {
  // The URL lives in HttpMessage's reason slot, whose public setter truncates reason phrases longer
  // than 1024 bytes. A request target (e.g. a large query string) must be stored intact.
  std::string longUrl("http://example.com/v1/orderbook?markets=");
  while (longUrl.size() <= 8000) {
    longUrl.append("KRW-BTC,");
  }

  ClientRequest req(http::Method::GET, longUrl);
  EXPECT_EQ(req.url(), longUrl);

  ClientRequest req2;
  req2.url(longUrl);
  EXPECT_EQ(req2.url(), longUrl);
}

TEST(ClientRequestTest, RvalueOverloadsCompose) {
  // Exercise every rvalue-qualified overload in a single chained temporary.
  ClientRequest req = ClientRequest()
                          .method(http::Method::PATCH)
                          .url("http://h/r")
                          .header("X-H", "v")
                          .headerAddLine("X-Add", "w")
                          .body("rbody");
  EXPECT_EQ(req.method(), http::Method::PATCH);
  EXPECT_EQ(req.url(), "http://h/r");
  EXPECT_EQ(req.body(), "rbody");

  ClientRequest typed = ClientRequest(http::Method::POST, "http://h/t").body("json", "application/json");
  EXPECT_EQ(typed.body(), "json");
}

}  // namespace aeronet
