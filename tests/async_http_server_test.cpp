#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "aeronet/async-http-server.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

TEST(AsyncHttpServer, BasicStartStopAndRequest) {
  aeronet::AsyncHttpServer async(aeronet::HttpServerConfig{});
  async.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest &req) {
    return aeronet::HttpResponse(aeronet::http::StatusCodeOK)
        .contentType(aeronet::http::ContentTypeTextPlain)
        .body("hello-async");
  });
  async.start();
  // Allow a short grace period.
  std::this_thread::sleep_for(20ms);
  auto port = async.port();
  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/";
  auto resp = aeronet::test::requestOrThrow(port, opt);
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("hello-async"));
}

TEST(AsyncHttpServer, PredicateStop) {
  std::atomic<bool> done{false};
  aeronet::AsyncHttpServer async(aeronet::HttpServerConfig{});
  async.router().setDefault([](const aeronet::HttpRequest &req) {
    return aeronet::HttpResponse(aeronet::http::StatusCodeOK).body(req.path());
  });
  async.startUntil([&] { return done.load(); });
  std::this_thread::sleep_for(15ms);  // let it spin
  auto port = async.port();
  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/xyz";
  auto resp = aeronet::test::requestOrThrow(port, opt);
  ASSERT_TRUE(resp.contains("/xyz"));
  done.store(true);
  // stop should be idempotent after predicate triggers stop
  async.stop();
  async.stop();
}

TEST(AsyncHttpServer, Restart) {
  aeronet::AsyncHttpServer async(aeronet::HttpServerConfig{});
  auto port = async.port();
  EXPECT_GT(port, 0);
  async.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest &req) {
    return aeronet::HttpResponse(aeronet::http::StatusCodeOK)
        .contentType(aeronet::http::ContentTypeTextPlain)
        .body("hello-async1");
  });
  async.start();
  // Allow a short grace period.
  std::this_thread::sleep_for(20ms);

  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/";

  auto resp = aeronet::test::requestOrThrow(port, opt);
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("hello-async1"));

  async.stop();
  // change router
  async.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest &req) {
    return aeronet::HttpResponse(aeronet::http::StatusCodeOK)
        .contentType(aeronet::http::ContentTypeTextPlain)
        .body("hello-async2");
  });
  async.start();

  resp = aeronet::test::requestOrThrow(port, opt);
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("hello-async2"));
}