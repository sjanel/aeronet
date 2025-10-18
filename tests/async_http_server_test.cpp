#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/async-http-server.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

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

bool WaitForServerRunning(const aeronet::AsyncHttpServer &server, std::chrono::milliseconds timeout) {
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
  async.startAndStopWhen([&] { return done.load(); });
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

TEST(AsyncHttpServer, StartWithStopToken) {
  aeronet::AsyncHttpServer async(aeronet::HttpServerConfig{});
  async.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest &req) {
    return aeronet::HttpResponse(aeronet::http::StatusCodeOK).body("token-ok");
  });

  std::stop_source src;
  async.startWithStopToken(src.get_token());
  // Allow startup
  std::this_thread::sleep_for(15ms);
  auto port = async.port();
  aeronet::test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/";
  auto resp = aeronet::test::requestOrThrow(port, opt);
  ASSERT_TRUE(resp.contains("token-ok"));

  // Request stop via the stop_source and then join via stop().
  src.request_stop();
  // joining the background thread via stop should succeed even if token already requested
  async.stop();
}

TEST(AsyncHttpServer, BeginDrainClosesKeepAliveConnections) {
  aeronet::HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  aeronet::AsyncHttpServer async(cfg);

  async.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest &req) {
    aeronet::HttpResponse resp;
    resp.body("OK");
    return resp;
  });

  async.start();
  ASSERT_TRUE(WaitForServerRunning(async, 1s));

  const auto port = async.port();
  aeronet::test::ClientConnection cnx(port);
  const int fd = cnx.fd();
  ASSERT_GE(fd, 0);

  const auto req = SimpleGetRequest("/", "keep-alive");
  ASSERT_TRUE(aeronet::test::sendAll(fd, req));
  const auto initial = aeronet::test::recvWithTimeout(fd);
  ASSERT_TRUE(initial.size() > 0);

  async.beginDrain(std::chrono::milliseconds{200});
  EXPECT_TRUE(async.isDraining());

  // Wait briefly for the listener to be closed by beginDrain() (avoid racey immediate connect attempts)
  EXPECT_TRUE(aeronet::test::WaitForListenerClosed(port, 200ms));

  const auto second = SimpleGetRequest("/two", "keep-alive");
  ASSERT_TRUE(aeronet::test::sendAll(fd, second));
  const auto drained = aeronet::test::recvWithTimeout(fd);
  EXPECT_TRUE(drained.contains("Connection: close"));

  EXPECT_TRUE(aeronet::test::WaitForPeerClose(fd, 500ms));

  async.stop();
  EXPECT_FALSE(async.isRunning());
}