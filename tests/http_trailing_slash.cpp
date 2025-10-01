#include <gtest/gtest.h>

#include <cstdint>  // uint16_t
#include <string>

#include "aeronet/http-request.hpp"   // aeronet::HttpRequest
#include "aeronet/http-response.hpp"  // aeronet::HttpResponse
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "http-method.hpp"
#include "test_http_client.hpp"
#include "test_server_fixture.hpp"

using aeronet::HttpServerConfig;

namespace {
std::string rawRequest(uint16_t port, const std::string& target) {
  test_http_client::RequestOptions opt;
  opt.method = "GET";
  opt.target = target;
  opt.connection = "close";
  auto resp = test_http_client::request(port, opt);
  return resp.value_or("");
}

}  // namespace

TEST(HttpTrailingSlash, StrictPolicyDifferent) {
  HttpServerConfig cfg{};
  cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Strict);
  TestServer ts(cfg);
  ts.server.addPathHandler("/alpha", aeronet::http::Method::GET,
                           [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("alpha"); });
  auto resp = rawRequest(ts.port(), "/alpha/");
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("404"));
}

TEST(HttpTrailingSlash, NormalizePolicyStrips) {
  HttpServerConfig cfg{};
  cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Normalize);
  TestServer ts(cfg);
  ts.server.addPathHandler("/beta", aeronet::http::Method::GET,
                           [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("beta"); });
  auto resp = rawRequest(ts.port(), "/beta/");
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("200"));
  ASSERT_NE(std::string::npos, resp.find("beta"));
}

TEST(HttpTrailingSlash, RedirectPolicy) {
  HttpServerConfig cfg{};
  cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Redirect);
  TestServer ts(cfg);
  ts.server.addPathHandler("/gamma", aeronet::http::Method::GET,
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
  cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Strict);
  TestServer ts(cfg);
  ts.server.addPathHandler("/sigma/", aeronet::http::Method::GET,
                           [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("sigma"); });
  auto ok = rawRequest(ts.port(), "/sigma/");
  auto notFound = rawRequest(ts.port(), "/sigma");
  ts.stop();
  ASSERT_NE(std::string::npos, ok.find("200"));
  ASSERT_NE(std::string::npos, notFound.find("404"));
}

TEST(HttpTrailingSlash, NormalizePolicyRegisteredWithSlashAcceptsWithout) {
  HttpServerConfig cfg{};
  cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Normalize);
  TestServer ts(cfg);
  ts.server.addPathHandler("/norm/", aeronet::http::Method::GET,
                           [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("norm"); });
  auto withSlash = rawRequest(ts.port(), "/norm/");
  auto withoutSlash = rawRequest(ts.port(), "/norm");
  ts.stop();
  ASSERT_NE(std::string::npos, withSlash.find("200"));
  ASSERT_NE(std::string::npos, withoutSlash.find("200"));
  ASSERT_NE(std::string::npos, withoutSlash.find("norm"));
}

TEST(HttpTrailingSlash, RedirectPolicyCanonicalOnlyMatchesWithoutSlash) {
  HttpServerConfig cfg{};
  cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Redirect);
  TestServer ts(cfg);
  ts.server.addPathHandler("/redir", aeronet::http::Method::GET,
                           [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("redir"); });
  auto redirect = rawRequest(ts.port(), "/redir/");  // should 301 -> /redir
  auto canonical = rawRequest(ts.port(), "/redir");  // should 200
  ts.stop();
  ASSERT_NE(std::string::npos, redirect.find("301"));
  ASSERT_NE(std::string::npos, redirect.find("Location: /redir\r\n"));
  ASSERT_NE(std::string::npos, canonical.find("200"));
  ASSERT_NE(std::string::npos, canonical.find("redir"));
}

TEST(HttpTrailingSlash, RedirectPolicyRegisteredWithSlashDoesNotAddSlash) {
  // Current behavior: if only "/only/" is registered and Redirect policy set, request to "/only" is 404 (no inverse
  // redirect)
  HttpServerConfig cfg{};
  cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Redirect);
  TestServer ts(cfg);
  ts.server.addPathHandler("/only/", aeronet::http::Method::GET,
                           [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("only"); });
  auto withSlash = rawRequest(ts.port(), "/only/");
  auto withoutSlash = rawRequest(ts.port(), "/only");
  ts.stop();
  ASSERT_NE(std::string::npos, withSlash.find("200"));
  ASSERT_NE(std::string::npos, withoutSlash.find("404"));
}

TEST(HttpTrailingSlash, RootPathNotRedirected) {
  HttpServerConfig cfg{};
  cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Redirect);
  TestServer ts(cfg);
  auto resp = rawRequest(ts.port(), "/");  // no handlers => 404 but not 301
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("404"));
  ASSERT_EQ(std::string::npos, resp.find("301"));
}

// ================= Additional tests for exact-match-first semantics =================

TEST(HttpTrailingSlash, RedirectPolicyBothVariants_NoRedirectWhenExact) {
  HttpServerConfig cfg{};
  cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Redirect);
  TestServer ts(cfg);
  ts.server.addPathHandler("/dual", aeronet::http::Method::GET,
                           [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("dual-no-slash"); });
  ts.server.addPathHandler("/dual/", aeronet::http::Method::GET,
                           [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("dual-with-slash"); });
  auto respNoSlash = rawRequest(ts.port(), "/dual");
  auto respWithSlash = rawRequest(ts.port(), "/dual/");
  ts.stop();
  ASSERT_NE(std::string::npos, respNoSlash.find("200"));
  ASSERT_NE(std::string::npos, respNoSlash.find("dual-no-slash"));
  ASSERT_NE(std::string::npos, respWithSlash.find("200"));
  ASSERT_NE(std::string::npos, respWithSlash.find("dual-with-slash"));
  // Crucially: no 301 redirect should appear because exact matches exist for both requests.
  ASSERT_EQ(std::string::npos, respWithSlash.find("301"));
}

TEST(HttpTrailingSlash, NormalizePolicyBothVariants_Independent) {
  HttpServerConfig cfg{};
  cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Normalize);
  TestServer ts(cfg);
  ts.server.addPathHandler("/sep", aeronet::http::Method::GET,
                           [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("sep-no-slash"); });
  ts.server.addPathHandler("/sep/", aeronet::http::Method::GET,
                           [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("sep-with-slash"); });
  auto respNoSlash = rawRequest(ts.port(), "/sep");
  auto respWithSlash = rawRequest(ts.port(), "/sep/");
  ts.stop();
  ASSERT_NE(std::string::npos, respNoSlash.find("200"));
  ASSERT_NE(std::string::npos, respNoSlash.find("sep-no-slash"));
  ASSERT_NE(std::string::npos, respWithSlash.find("200"));
  ASSERT_NE(std::string::npos, respWithSlash.find("sep-with-slash"));
}

TEST(HttpTrailingSlash, StrictPolicyBothVariants_Independent) {
  HttpServerConfig cfg{};
  cfg.withTrailingSlashPolicy(HttpServerConfig::TrailingSlashPolicy::Strict);
  TestServer ts(cfg);
  ts.server.addPathHandler("/both", aeronet::http::Method::GET,
                           [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("both-no-slash"); });
  ts.server.addPathHandler("/both/", aeronet::http::Method::GET,
                           [](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("both-with-slash"); });
  auto respNoSlash = rawRequest(ts.port(), "/both");
  auto respWithSlash = rawRequest(ts.port(), "/both/");
  ts.stop();
  ASSERT_NE(std::string::npos, respNoSlash.find("200"));
  ASSERT_NE(std::string::npos, respNoSlash.find("both-no-slash"));
  ASSERT_NE(std::string::npos, respWithSlash.find("200"));
  ASSERT_NE(std::string::npos, respWithSlash.find("both-with-slash"));
}
