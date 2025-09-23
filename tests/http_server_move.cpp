#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "test_raw_get.hpp"

TEST(HttpServerMove, MoveConstructAndServe) {
  std::atomic_bool stop{false};
  aeronet::HttpServer original(aeronet::ServerConfig{});
  auto port = original.port();
  original.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = std::string("ORIG:") + std::string(req.target);
    return resp;
  });

  // Move construct server before running
  aeronet::HttpServer moved(std::move(original));

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }, std::chrono::milliseconds(50)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::string resp;
  test_helpers::rawGet(port, "/mv", resp);
  stop.store(true);

  ASSERT_NE(std::string::npos, resp.find("ORIG:/mv"));
}

TEST(HttpServerMove, MoveAssignWhileStopped) {
  aeronet::HttpServer s1(aeronet::ServerConfig{}.withReusePort(false));
  aeronet::HttpServer s2(aeronet::ServerConfig{}.withReusePort(false));
  auto port1 = s1.port();
  auto port2 = s2.port();

  EXPECT_NE(port1, port2);

  s1.setHandler([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = "S1";
    return resp;
  });
  s2.setHandler([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = "S2";
    return resp;
  });

  // Move assign s1 <- s2 (both stopped)
  s1 = std::move(s2);
  EXPECT_EQ(s1.port(), port2);

  std::atomic_bool stop{false};
  std::jthread th([&] { s1.runUntil([&] { return stop.load(); }, std::chrono::milliseconds(50)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  std::string resp;
  test_helpers::rawGet(port2, "/x", resp);
  stop.store(true);
  ASSERT_NE(std::string::npos, resp.find("S2"));
}
