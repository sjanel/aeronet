#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "test_http_client.hpp"
#include "test_server_fixture.hpp"

using namespace std::chrono_literals;

namespace {
std::string httpGet(uint16_t port, const std::string& target) {
  test_http_client::RequestOptions opt;
  opt.method = "GET";
  opt.target = target;
  opt.connection = "close";
  opt.headers.emplace_back("X-Test", "abc123");
  auto resp = test_http_client::request(port, opt);
  return resp.value_or("");
}
}  // namespace

TEST(HttpBasic, SimpleGet) {
  TestServer ts(aeronet::ServerConfig{});
  ts.server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    auto testHeaderIt = req.headers.find("X-Test");
    resp.body = std::string("You requested: ") + std::string(req.target);
    if (testHeaderIt != req.headers.end() && !testHeaderIt->second.empty()) {
      resp.body += ", X-Test=";
      resp.body.append(testHeaderIt->second);
    }
    return resp;
  });
  std::string resp = httpGet(ts.port(), "/abc");
  ts.stop();
  ASSERT_FALSE(resp.empty());
  ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
  ASSERT_NE(std::string::npos, resp.find("You requested: /abc"));
  ASSERT_NE(std::string::npos, resp.find("X-Test=abc123"));
}
