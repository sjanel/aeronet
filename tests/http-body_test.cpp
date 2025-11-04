#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

namespace {
std::string httpGet(uint16_t port, const std::string& target) {
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = target;
  opt.connection = "close";
  opt.headers.emplace_back("X-Test", "abc123");
  auto resp = test::request(port, opt);
  return resp.value_or("");
}
}  // namespace

TEST(HttpBasic, SimpleGet) {
  test::TestServer ts(HttpServerConfig{});
  ts.server.router().setDefault([](const HttpRequest& req) {
    HttpResponse resp;
    auto testHeaderIt = req.headers().find("X-Test");
    std::string body("You requested: ");
    body += req.path();
    if (testHeaderIt != req.headers().end() && !testHeaderIt->second.empty()) {
      body += ", X-Test=";
      body.append(testHeaderIt->second);
    }
    resp.body(std::move(body));
    return resp;
  });
  std::string resp = httpGet(ts.port(), "/abc");
  ASSERT_FALSE(resp.empty());
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("You requested: /abc"));
  ASSERT_TRUE(resp.contains("X-Test=abc123"));
}
