#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

namespace {
test::TestServer ts(HttpServerConfig{});
}

TEST(HttpRouting, BasicPathDispatch) {
  ts.server.router().setPath(http::Method::GET, "/hello", [](const HttpRequest&) {
    return HttpResponse(http::StatusCodeOK, "OK").body("world").contentType(aeronet::http::ContentTypeTextPlain);
  });
  ts.server.router().setPath(http::Method::GET | http::Method::POST, "/multi", [](const HttpRequest& req) {
    return HttpResponse(http::StatusCodeOK, "OK")
        .body(std::string(http::toMethodStr(req.method())) + "!")
        .contentType(aeronet::http::ContentTypeTextPlain);
  });

  aeronet::test::RequestOptions getHello;
  getHello.method = "GET";
  getHello.target = "/hello";
  auto resp1 = aeronet::test::requestOrThrow(ts.port(), getHello);
  EXPECT_TRUE(resp1.contains("200 OK"));
  EXPECT_TRUE(resp1.contains("world"));
  aeronet::test::RequestOptions postHello;
  postHello.method = "POST";
  postHello.target = "/hello";
  postHello.headers.emplace_back("Content-Length", "0");
  auto resp2 = aeronet::test::requestOrThrow(ts.port(), postHello);
  EXPECT_TRUE(resp2.contains("405 Method Not Allowed"));
  aeronet::test::RequestOptions getMissing;
  getMissing.method = "GET";
  getMissing.target = "/missing";
  auto resp3 = aeronet::test::requestOrThrow(ts.port(), getMissing);
  EXPECT_TRUE(resp3.contains("404 Not Found"));
  aeronet::test::RequestOptions postMulti;
  postMulti.method = "POST";
  postMulti.target = "/multi";
  postMulti.headers.emplace_back("Content-Length", "0");
  auto resp4 = aeronet::test::requestOrThrow(ts.port(), postMulti);
  EXPECT_TRUE(resp4.contains("200 OK"));
  EXPECT_TRUE(resp4.contains("POST!"));
}

TEST(HttpRouting, GlobalFallbackWithPathHandlers) {
  ts.server.router().setDefault([](const HttpRequest&) { return HttpResponse(200, "OK"); });
  // Adding path handler after global handler is now allowed (Phase 2 mixing model)
  EXPECT_NO_THROW(
      ts.server.router().setPath(http::Method::GET, "/x", [](const HttpRequest&) { return HttpResponse(200); }));
}

TEST(HttpRouting, PathParametersInjectedIntoRequest) {
  std::string seenUser;
  std::string seenPost;
  ts.server.router().setPath(http::Method::GET, "/users/{userId}/posts/{postId}", [&](const HttpRequest& req) {
    const auto& params = req.pathParams();
    if (const auto itUser = params.find("userId"); itUser != params.end()) {
      seenUser.assign(itUser->second);
    }
    if (const auto itPost = params.find("postId"); itPost != params.end()) {
      seenPost.assign(itPost->second);
    }
    return HttpResponse(200, "OK").contentType(http::ContentTypeTextPlain).body("ok");
  });

  aeronet::test::RequestOptions reqOpts;
  reqOpts.method = "GET";
  reqOpts.target = "/users/42/posts/abcd";
  auto resp = aeronet::test::requestOrThrow(ts.server.port(), reqOpts);
  EXPECT_TRUE(resp.contains("200 OK"));
  EXPECT_EQ(seenUser, "42");
  EXPECT_EQ(seenPost, "abcd");
}

namespace {
std::string rawRequest(uint16_t port, const std::string& target) {
  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = target;
  opt.connection = "close";
  auto resp = aeronet::test::request(port, opt);
  return resp.value_or("");
}

}  // namespace

class HttpTrailingSlash : public ::testing::Test {
 protected:
  static test::TestServer createTestServer(RouterConfig::TrailingSlashPolicy trailingSlashPolicy) {
    RouterConfig routerCfg;
    routerCfg.withTrailingSlashPolicy(trailingSlashPolicy);
    return test::TestServer(HttpServerConfig{}, std::move(routerCfg));
  }

  test::TestServer normTestServer{createTestServer(RouterConfig::TrailingSlashPolicy::Normalize)};
  test::TestServer strictTestServer{createTestServer(RouterConfig::TrailingSlashPolicy::Strict)};
  test::TestServer redirectTestServer{createTestServer(RouterConfig::TrailingSlashPolicy::Redirect)};
};

TEST_F(HttpTrailingSlash, StrictPolicyDifferent) {
  strictTestServer.server.router().setPath(aeronet::http::Method::GET, "/alpha", [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse().body("alpha");
  });
  auto resp = rawRequest(strictTestServer.port(), "/alpha/");
  ASSERT_TRUE(resp.contains("404"));
}

TEST_F(HttpTrailingSlash, NormalizePolicyStrips) {
  normTestServer.server.router().setPath(aeronet::http::Method::GET, "/beta", [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse().body("beta");
  });
  auto resp = rawRequest(normTestServer.port(), "/beta/");
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("beta"));
}

TEST_F(HttpTrailingSlash, NormalizePolicyAddSlash) {
  normTestServer.server.router().setPath(aeronet::http::Method::GET, "/beta/", [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse().body("beta/");
  });
  auto resp = rawRequest(normTestServer.port(), "/beta");

  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("beta"));
}

TEST_F(HttpTrailingSlash, RedirectPolicy) {
  redirectTestServer.server.router().setPath(aeronet::http::Method::GET, "/gamma", [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse().body("gamma");
  });
  auto resp = rawRequest(redirectTestServer.port(), "/gamma/");
  // Expect 301 and Location header
  ASSERT_TRUE(resp.contains("301"));
  ASSERT_TRUE(resp.contains("Location: /gamma\r\n"));
}

// Additional matrix coverage

TEST_F(HttpTrailingSlash, StrictPolicyRegisteredWithSlashDoesNotMatchWithout) {
  strictTestServer.server.router().setPath(aeronet::http::Method::GET, "/sigma/", [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse().body("sigma");
  });
  auto ok = rawRequest(strictTestServer.port(), "/sigma/");
  auto notFound = rawRequest(strictTestServer.port(), "/sigma");
  ASSERT_TRUE(ok.contains("200"));
  ASSERT_TRUE(notFound.contains("404"));
}

TEST_F(HttpTrailingSlash, NormalizePolicyRegisteredWithSlashAcceptsWithout) {
  normTestServer.server.router().setPath(aeronet::http::Method::GET, "/norm/", [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse().body("norm");
  });
  auto withSlash = rawRequest(normTestServer.port(), "/norm/");
  auto withoutSlash = rawRequest(normTestServer.port(), "/norm");
  ASSERT_TRUE(withSlash.contains("200"));
  ASSERT_TRUE(withoutSlash.contains("200"));
  ASSERT_TRUE(withoutSlash.contains("norm"));
}

TEST_F(HttpTrailingSlash, RedirectPolicyRemoveSlash) {
  redirectTestServer.server.router().setPath(aeronet::http::Method::GET, "/redir", [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse().body("redir");
  });
  auto redirect = rawRequest(redirectTestServer.port(), "/redir/");  // should 301 -> /redir
  auto canonical = rawRequest(redirectTestServer.port(), "/redir");  // should 200
  ASSERT_TRUE(redirect.contains("301"));
  ASSERT_TRUE(redirect.contains("Location: /redir\r\n"));
  ASSERT_TRUE(canonical.contains("200"));
  ASSERT_TRUE(canonical.contains("redir"));
}

TEST_F(HttpTrailingSlash, RedirectPolicyAddSlash) {
  redirectTestServer.server.router().setPath(aeronet::http::Method::GET, "/only/", [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse().body("only");
  });
  auto withSlash = rawRequest(redirectTestServer.port(), "/only/");
  auto withoutSlash = rawRequest(redirectTestServer.port(), "/only");

  ASSERT_TRUE(withSlash.contains("200"));
  ASSERT_TRUE(withoutSlash.contains("301"));
}

TEST_F(HttpTrailingSlash, RootPathNotRedirected) {
  auto resp = rawRequest(redirectTestServer.port(), "/");  // no handlers => 404 but not 301
  ASSERT_TRUE(resp.contains("404"));
  ASSERT_FALSE(resp.contains("301"));
}

TEST_F(HttpTrailingSlash, StrictPolicyBothVariants_Independent) {
  strictTestServer.server.router().setPath(aeronet::http::Method::GET, "/both", [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse().body("both-no-slash");
  });
  strictTestServer.server.router().setPath(aeronet::http::Method::GET, "/both/", [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse().body("both-with-slash");
  });
  auto respNoSlash = rawRequest(strictTestServer.port(), "/both");
  auto respWithSlash = rawRequest(strictTestServer.port(), "/both/");

  ASSERT_TRUE(respNoSlash.contains("200"));
  ASSERT_TRUE(respNoSlash.contains("both-no-slash"));
  ASSERT_TRUE(respWithSlash.contains("200"));
  ASSERT_TRUE(respWithSlash.contains("both-with-slash"));
}
