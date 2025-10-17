#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"

TEST(HttpServerRestart, RestartPossible) {
  std::atomic_bool stop1{false};
  std::atomic_bool stop2{false};
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  auto port = server.port();
  server.router().setDefault([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body(std::string("ORIG:") + std::string(req.path()));
    return resp;
  });

  std::jthread th([&] {
    server.runUntil([&] { return stop1.load(); });
    server.runUntil([&] { return stop2.load(); });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::string resp = aeronet::test::simpleGet(port, "/mv");

  ASSERT_TRUE(resp.contains("ORIG:/mv"));

  stop1.store(true);

  // Should start a second time, same port.
  EXPECT_EQ(port, server.port());

  resp = aeronet::test::simpleGet(port, "/mv2");
  ASSERT_TRUE(resp.contains("ORIG:/mv2"));
  stop2.store(true);
}
