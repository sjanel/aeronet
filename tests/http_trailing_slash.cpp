#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

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

TEST(HttpTrailingSlash, StrictPolicyDifferent) {
  HttpServerConfig cfg{};
  cfg.router.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setPath("/alpha", aeronet::http::Method::GET,
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("alpha"); });
  auto resp = rawRequest(ts.port(), "/alpha/");
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("404"));
}

TEST(HttpTrailingSlash, NormalizePolicyStrips) {
  HttpServerConfig cfg{};
  cfg.router.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setPath("/beta", aeronet::http::Method::GET,
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("beta"); });
  auto resp = rawRequest(ts.port(), "/beta/");
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("200"));
  ASSERT_NE(std::string::npos, resp.find("beta"));
}

TEST(HttpTrailingSlash, NormalizePolicyAddSlash) {
  HttpServerConfig cfg{};
  cfg.router.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setPath("/beta/", aeronet::http::Method::GET,
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("beta/"); });
  auto resp = rawRequest(ts.port(), "/beta");
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("200"));
  ASSERT_NE(std::string::npos, resp.find("beta"));
}

TEST(HttpTrailingSlash, RedirectPolicy) {
  HttpServerConfig cfg{};
  cfg.router.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setPath("/gamma", aeronet::http::Method::GET,
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("gamma"); });
  auto resp = rawRequest(ts.port(), "/gamma/");
  ts.stop();
  // Expect 301 and Location header
  ASSERT_NE(std::string::npos, resp.find("301"));
  ASSERT_NE(std::string::npos, resp.find("Location: /gamma\r\n"));
}

// Additional matrix coverage

TEST(HttpTrailingSlash, StrictPolicyRegisteredWithSlashDoesNotMatchWithout) {
  HttpServerConfig cfg{};
  cfg.router.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setPath("/sigma/", aeronet::http::Method::GET,
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("sigma"); });
  auto ok = rawRequest(ts.port(), "/sigma/");
  auto notFound = rawRequest(ts.port(), "/sigma");
  ts.stop();
  ASSERT_NE(std::string::npos, ok.find("200"));
  ASSERT_NE(std::string::npos, notFound.find("404"));
}

TEST(HttpTrailingSlash, NormalizePolicyRegisteredWithSlashAcceptsWithout) {
  HttpServerConfig cfg{};
  cfg.router.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setPath("/norm/", aeronet::http::Method::GET,
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("norm"); });
  auto withSlash = rawRequest(ts.port(), "/norm/");
  auto withoutSlash = rawRequest(ts.port(), "/norm");
  ts.stop();
  ASSERT_NE(std::string::npos, withSlash.find("200"));
  ASSERT_NE(std::string::npos, withoutSlash.find("200"));
  ASSERT_NE(std::string::npos, withoutSlash.find("norm"));
}

TEST(HttpTrailingSlash, RedirectPolicyRemoveSlash) {
  HttpServerConfig cfg{};
  cfg.router.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setPath("/redir", aeronet::http::Method::GET,
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("redir"); });
  auto redirect = rawRequest(ts.port(), "/redir/");  // should 301 -> /redir
  auto canonical = rawRequest(ts.port(), "/redir");  // should 200
  ASSERT_NE(std::string::npos, redirect.find("301"));
  ASSERT_NE(std::string::npos, redirect.find("Location: /redir\r\n"));
  ASSERT_NE(std::string::npos, canonical.find("200"));
  ASSERT_NE(std::string::npos, canonical.find("redir"));
}

TEST(HttpTrailingSlash, RedirectPolicyAddSlash) {
  HttpServerConfig cfg{};
  cfg.router.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setPath("/only/", aeronet::http::Method::GET,
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("only"); });
  auto withSlash = rawRequest(ts.port(), "/only/");
  auto withoutSlash = rawRequest(ts.port(), "/only");

  ASSERT_NE(std::string::npos, withSlash.find("200"));
  ASSERT_NE(std::string::npos, withoutSlash.find("301"));
}

TEST(HttpTrailingSlash, RootPathNotRedirected) {
  HttpServerConfig cfg{};
  cfg.router.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  aeronet::test::TestServer ts(cfg);
  auto resp = rawRequest(ts.port(), "/");  // no handlers => 404 but not 301
  ASSERT_NE(std::string::npos, resp.find("404"));
  ASSERT_EQ(std::string::npos, resp.find("301"));
}

TEST(HttpTrailingSlash, StrictPolicyBothVariants_Independent) {
  HttpServerConfig cfg{};
  cfg.router.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setPath("/both", aeronet::http::Method::GET,
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("both-no-slash"); });
  ts.server.router().setPath("/both/", aeronet::http::Method::GET, [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse().body("both-with-slash");
  });
  auto respNoSlash = rawRequest(ts.port(), "/both");
  auto respWithSlash = rawRequest(ts.port(), "/both/");
  ts.stop();
  ASSERT_NE(std::string::npos, respNoSlash.find("200"));
  ASSERT_NE(std::string::npos, respNoSlash.find("both-no-slash"));
  ASSERT_NE(std::string::npos, respWithSlash.find("200"));
  ASSERT_NE(std::string::npos, respWithSlash.find("both-with-slash"));
}
