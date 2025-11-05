#include "aeronet/async-http-server.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

namespace {

std::string SimpleGetRequest(std::string_view target, std::string_view connectionHeader = "close") {
  std::string req;
  req.reserve(128);
  req.append("GET ").append(target).append(" HTTP/1.1\r\n");
  req.append("Host: localhost\r\n");
  req.append("Connection: ").append(connectionHeader).append("\r\n");
  req.append("Content-Length: 0\r\n\r\n");
  return req;
}

bool WaitForServerRunning(const AsyncHttpServer &server, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
    if (server.isRunning()) {
      return true;
    }
  }
  return server.isRunning();
}

}  // namespace

TEST(AsyncHttpServer, BasicStartStopAndRequest) {
  AsyncHttpServer async(HttpServerConfig{});
  async.router().setDefault(
      []([[maybe_unused]] const HttpRequest &req) { return HttpResponse(http::StatusCodeOK).body("hello-async"); });
  async.start();
  // Allow a short grace period.
  std::this_thread::sleep_for(20ms);
  auto port = async.port();
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/";
  auto resp = test::requestOrThrow(port, opt);
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("hello-async"));
}

TEST(AsyncHttpServer, PredicateStop) {
  std::atomic<bool> done{false};
  AsyncHttpServer async(HttpServerConfig{});
  async.router().setDefault([](const HttpRequest &req) { return HttpResponse(http::StatusCodeOK).body(req.path()); });
  async.startAndStopWhen([&] { return done.load(); });
  std::this_thread::sleep_for(15ms);  // let it spin
  auto port = async.port();
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/xyz";
  auto resp = test::requestOrThrow(port, opt);
  ASSERT_TRUE(resp.contains("/xyz"));
  done.store(true);
  // stop should be idempotent after predicate triggers stop
  async.stop();
  async.stop();
}

TEST(AsyncHttpServer, Restart) {
  AsyncHttpServer async(HttpServerConfig{});
  auto port = async.port();
  EXPECT_GT(port, 0);
  async.router().setDefault(
      []([[maybe_unused]] const HttpRequest &req) { return HttpResponse(http::StatusCodeOK).body("hello-async1"); });
  async.start();
  // Allow a short grace period.
  std::this_thread::sleep_for(20ms);

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/";

  auto resp = test::requestOrThrow(port, opt);
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("hello-async1"));

  async.stop();
  // change router
  async.router().setDefault(
      []([[maybe_unused]] const HttpRequest &req) { return HttpResponse(http::StatusCodeOK).body("hello-async2"); });
  async.start();

  resp = test::requestOrThrow(port, opt);
  ASSERT_TRUE(resp.contains("200"));
  ASSERT_TRUE(resp.contains("hello-async2"));
}

TEST(AsyncHttpServer, StartWithStopToken) {
  AsyncHttpServer async(HttpServerConfig{});
  async.router().setDefault(
      []([[maybe_unused]] const HttpRequest &req) { return HttpResponse(http::StatusCodeOK).body("token-ok"); });

  std::stop_source src;
  async.startWithStopToken(src.get_token());
  // Allow startup
  std::this_thread::sleep_for(15ms);
  auto port = async.port();
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/";
  auto resp = test::requestOrThrow(port, opt);
  ASSERT_TRUE(resp.contains("token-ok"));

  // Request stop via the stop_source and then join via stop().
  src.request_stop();
  // joining the background thread via stop should succeed even if token already requested
  async.stop();
}

TEST(AsyncHttpServer, BeginDrainClosesKeepAliveConnections) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  AsyncHttpServer async(cfg);

  async.router().setDefault([]([[maybe_unused]] const HttpRequest &req) {
    HttpResponse resp;
    resp.body("OK");
    return resp;
  });

  async.start();
  ASSERT_TRUE(WaitForServerRunning(async, 1s));

  const auto port = async.port();
  test::ClientConnection cnx(port);
  const int fd = cnx.fd();
  ASSERT_GE(fd, 0);

  const auto req = SimpleGetRequest("/", "keep-alive");
  test::sendAll(fd, req);
  const auto initial = test::recvWithTimeout(fd);
  ASSERT_FALSE(initial.empty());

  async.beginDrain(200ms);
  EXPECT_TRUE(async.isDraining());

  // Wait briefly for the listener to be closed by beginDrain() (avoid racey immediate connect attempts)
  EXPECT_TRUE(test::WaitForListenerClosed(port, 210ms));

  const auto second = SimpleGetRequest("/two", "keep-alive");
  test::sendAll(fd, second);
  const auto drained = test::recvWithTimeout(fd);
  EXPECT_TRUE(drained.contains("Connection: close"));

  EXPECT_TRUE(test::WaitForPeerClose(fd, 500ms));

  async.stop();
  EXPECT_FALSE(async.isRunning());
}