#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/async-http-server.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "test_http_client.hpp"

using namespace std::chrono_literals;

TEST(AsyncHttpServer, BasicStartStopAndRequest) {
  // Build a server with ephemeral port (0).
  aeronet::HttpServer base(aeronet::HttpServerConfig{});
  base.setHandler([]([[maybe_unused]] const aeronet::HttpRequest &req) {
    return aeronet::HttpResponse(200).contentType(aeronet::http::ContentTypeTextPlain).body("hello-async");
  });
  aeronet::AsyncHttpServer async(std::move(base));
  async.start();
  // Allow a short grace period.
  std::this_thread::sleep_for(20ms);
  auto port = async.server().port();
  test_http_client::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/";
  auto resp = test_http_client::request_or_throw(port, opt);
  ASSERT_NE(resp.find("200"), std::string::npos);
  ASSERT_NE(resp.find("hello-async"), std::string::npos);
  async.requestStop();
  async.stopAndJoin();
}

TEST(AsyncHttpServer, PredicateStop) {
  std::atomic<bool> done{false};
  aeronet::AsyncHttpServer async = aeronet::AsyncHttpServer::makeFromConfig(aeronet::HttpServerConfig{});
  async.server().setHandler(
      [](const aeronet::HttpRequest &req) { return aeronet::HttpResponse(200).body(req.path()); });
  async.startUntil([&] { return done.load(); });
  std::this_thread::sleep_for(15ms);  // let it spin
  auto port = async.server().port();
  test_http_client::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/xyz";
  auto resp = test_http_client::request_or_throw(port, opt);
  ASSERT_NE(resp.find("/xyz"), std::string::npos);
  done.store(true);
  // stopAndJoin should be idempotent after predicate triggers stop
  async.stopAndJoin();
}
