#include <gtest/gtest.h>

#include <cstdint>  // uint16_t
#include <string>   // std::string
#include <utility>  // std::move

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

namespace {
std::string httpGet(uint16_t port, const std::string& target) {
  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = target;
  opt.connection = "close";
  opt.headers.emplace_back("X-Test", "abc123");
  auto resp = aeronet::test::request(port, opt);
  return resp.value_or("");
}
}  // namespace

TEST(HttpBasic, SimpleGet) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
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
  ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
  ASSERT_NE(std::string::npos, resp.find("You requested: /abc"));
  ASSERT_NE(std::string::npos, resp.find("X-Test=abc123"));
}
