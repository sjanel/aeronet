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

class HttpTrailingSlash : public ::testing::Test {
 protected:
  void SetUp() override {}
  void TearDown() override {}

  HttpServerConfig cfg;
  RouterConfig routerCfg;
};

TEST_F(HttpTrailingSlash, StrictPolicyDifferent) {
  routerCfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  aeronet::test::TestServer ts(cfg, routerCfg);
  ts.server.router().setPath(aeronet::http::Method::GET, "/alpha",
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("alpha"); });
  auto resp = rawRequest(ts.port(), "/alpha/");
  ts.stop();
  ASSERT_TRUE(resp.contains("404"));
}

TEST_F(HttpTrailingSlash, NormalizePolicyStrips) {
  routerCfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize);
  aeronet::test::TestServer ts(cfg, routerCfg);
  ts.server.router().setPath(aeronet::http::Method::GET, "/beta",
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("beta"); });
  auto resp = rawRequest(ts.port(), "/beta/");
  ts.stop();
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("beta"));
}

TEST_F(HttpTrailingSlash, NormalizePolicyAddSlash) {
  routerCfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize);
  aeronet::test::TestServer ts(cfg, routerCfg);
  ts.server.router().setPath(aeronet::http::Method::GET, "/beta/",
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("beta/"); });
  auto resp = rawRequest(ts.port(), "/beta");
  ts.stop();
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("beta"));
}

TEST_F(HttpTrailingSlash, RedirectPolicy) {
  routerCfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  aeronet::test::TestServer ts(cfg, routerCfg);
  ts.server.router().setPath(aeronet::http::Method::GET, "/gamma",
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("gamma"); });
  auto resp = rawRequest(ts.port(), "/gamma/");
  ts.stop();
  // Expect 301 and Location header
  ASSERT_TRUE(resp.contains("301"));
  ASSERT_TRUE(resp.contains("Location: /gamma\r\n"));
}

// Additional matrix coverage

TEST_F(HttpTrailingSlash, StrictPolicyRegisteredWithSlashDoesNotMatchWithout) {
  routerCfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  aeronet::test::TestServer ts(cfg, routerCfg);
  ts.server.router().setPath(aeronet::http::Method::GET, "/sigma/",
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("sigma"); });
  auto ok = rawRequest(ts.port(), "/sigma/");
  auto notFound = rawRequest(ts.port(), "/sigma");
  ts.stop();
  ASSERT_TRUE(ok.contains("200"));
  ASSERT_TRUE(notFound.contains("404"));
}

TEST_F(HttpTrailingSlash, NormalizePolicyRegisteredWithSlashAcceptsWithout) {
  routerCfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Normalize);
  aeronet::test::TestServer ts(cfg, routerCfg);
  ts.server.router().setPath(aeronet::http::Method::GET, "/norm/",
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("norm"); });
  auto withSlash = rawRequest(ts.port(), "/norm/");
  auto withoutSlash = rawRequest(ts.port(), "/norm");
  ts.stop();
  ASSERT_TRUE(withSlash.contains("200"));
  ASSERT_TRUE(withoutSlash.contains("200"));
  ASSERT_TRUE(withoutSlash.contains("norm"));
}

TEST_F(HttpTrailingSlash, RedirectPolicyRemoveSlash) {
  routerCfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  aeronet::test::TestServer ts(cfg, routerCfg);
  ts.server.router().setPath(aeronet::http::Method::GET, "/redir",
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("redir"); });
  auto redirect = rawRequest(ts.port(), "/redir/");  // should 301 -> /redir
  auto canonical = rawRequest(ts.port(), "/redir");  // should 200
  ts.stop();
  ASSERT_TRUE(redirect.contains("301"));
  ASSERT_TRUE(redirect.contains("Location: /redir\r\n"));
  ASSERT_TRUE(canonical.contains("200"));
  ASSERT_TRUE(canonical.contains("redir"));
}

TEST_F(HttpTrailingSlash, RedirectPolicyAddSlash) {
  routerCfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  aeronet::test::TestServer ts(cfg, routerCfg);
  ts.server.router().setPath(aeronet::http::Method::GET, "/only/",
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("only"); });
  auto withSlash = rawRequest(ts.port(), "/only/");
  auto withoutSlash = rawRequest(ts.port(), "/only");
  ts.stop();

  ASSERT_TRUE(withSlash.contains("200"));
  ASSERT_TRUE(withoutSlash.contains("301"));
}

TEST_F(HttpTrailingSlash, RootPathNotRedirected) {
  routerCfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect);
  aeronet::test::TestServer ts(cfg, routerCfg);
  auto resp = rawRequest(ts.port(), "/");  // no handlers => 404 but not 301
  ts.stop();
  ASSERT_TRUE(resp.contains("404"));
  ASSERT_FALSE(resp.contains("301"));
}

TEST_F(HttpTrailingSlash, StrictPolicyBothVariants_Independent) {
  routerCfg.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Strict);
  aeronet::test::TestServer ts(cfg, routerCfg);
  ts.server.router().setPath(aeronet::http::Method::GET, "/both",
                             [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("both-no-slash"); });
  ts.server.router().setPath(aeronet::http::Method::GET, "/both/", [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse().body("both-with-slash");
  });
  auto respNoSlash = rawRequest(ts.port(), "/both");
  auto respWithSlash = rawRequest(ts.port(), "/both/");
  ts.stop();
  ASSERT_TRUE(respNoSlash.contains("200"));
  ASSERT_TRUE(respNoSlash.contains("both-no-slash"));
  ASSERT_TRUE(respWithSlash.contains("200"));
  ASSERT_TRUE(respWithSlash.contains("both-with-slash"));
}
