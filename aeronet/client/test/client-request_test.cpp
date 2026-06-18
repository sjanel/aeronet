// Unit coverage for ClientRequest: the fluent (lvalue & rvalue) builder API and the getters. No sockets.
#include "aeronet/client-request.hpp"

#include <gtest/gtest.h>

#include "aeronet/http-method.hpp"

namespace aeronet {

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
