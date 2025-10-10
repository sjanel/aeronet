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
  async.server().setHandler([]([[maybe_unused]] const aeronet::HttpRequest &req) {
    return aeronet::HttpResponse(aeronet::http::StatusCodeOK)
        .contentType(aeronet::http::ContentTypeTextPlain)
        .body("hello-async");
  });
  async.start();
  // Allow a short grace period.
  std::this_thread::sleep_for(20ms);
  auto port = async.server().port();
  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/";
  auto resp = aeronet::test::requestOrThrow(port, opt);
  ASSERT_NE(resp.find("200"), std::string::npos);
  ASSERT_NE(resp.find("hello-async"), std::string::npos);
}

TEST(AsyncHttpServer, PredicateStop) {
  std::atomic<bool> done{false};
  aeronet::AsyncHttpServer async(aeronet::HttpServerConfig{});
  async.server().setHandler([](const aeronet::HttpRequest &req) {
    return aeronet::HttpResponse(aeronet::http::StatusCodeOK).body(req.path());
  });
  async.startUntil([&] { return done.load(); });
  std::this_thread::sleep_for(15ms);  // let it spin
  auto port = async.server().port();
  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/xyz";
  auto resp = aeronet::test::requestOrThrow(port, opt);
  ASSERT_NE(resp.find("/xyz"), std::string::npos);
  done.store(true);
  // stop should be idempotent after predicate triggers stop
  async.stop();
  async.stop();
}
