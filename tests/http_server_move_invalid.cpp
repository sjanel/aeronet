#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"

// Validates that moving a running HttpServer (move-construction or move-assignment) throws std::runtime_error
// per the documented semantics (moves only allowed while stopped).

TEST(HttpServer, MoveConstructWhileRunningThrows) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("ok");
    return resp;
  });
  std::atomic<bool> go{false};
  std::jthread thr([&] {
    go = true;
    server.run();
  });
  // wait until thread entered run loop
  while (!go.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // Give a small slice for run() to set _running=true
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_THROW({ aeronet::HttpServer moved(std::move(server)); }, std::runtime_error);
}

TEST(HttpServer, MoveAssignWhileRunningThrows) {
  aeronet::HttpServerConfig cfg;
  aeronet::HttpServer serverA(cfg);
  aeronet::HttpServer serverB(cfg);
  serverA.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("a");
    return resp;
  });
  serverB.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("b");
    return resp;
  });
  std::atomic<bool> go{false};
  std::jthread thr([&] {
    go = true;
    serverA.run();
  });
  while (!go.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_THROW({ serverB = std::move(serverA); }, std::runtime_error);
}
