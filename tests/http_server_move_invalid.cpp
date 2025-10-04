#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"

// Validates that moving a running HttpServer (move-construction or move-assignment) throws std::runtime_error
// per the documented semantics (moves only allowed while stopped).

TEST(HttpServer, MoveConstructWhileRunningThrows) {
  aeronet::HttpServerConfig cfg;
  cfg.port = 0;  // ephemeral
  aeronet::HttpServer s(cfg);
  s.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse r;
    r.body("ok");
    return r;
  });
  std::atomic<bool> go{false};
  std::jthread thr([&] {
    go = true;
    s.run();
  });
  // wait until thread entered run loop
  while (!go.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // Give a small slice for run() to set _running=true
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_THROW({ aeronet::HttpServer moved(std::move(s)); }, std::runtime_error);
  s.stop();
}

TEST(HttpServer, MoveAssignWhileRunningThrows) {
  aeronet::HttpServerConfig cfgA;
  cfgA.port = 0;
  aeronet::HttpServerConfig cfgB;
  cfgB.port = 0;
  aeronet::HttpServer a(cfgA);
  aeronet::HttpServer b(cfgB);
  a.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse r;
    r.body("a");
    return r;
  });
  b.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse r;
    r.body("b");
    return r;
  });
  std::atomic<bool> go{false};
  std::jthread thr([&] {
    go = true;
    a.run();
  });
  while (!go.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_THROW({ b = std::move(a); }, std::runtime_error);
  a.stop();
}
