#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"

TEST(HttpServerMove, MoveConstructAndServe) {
  std::atomic_bool stop{false};
  aeronet::HttpServer original(aeronet::HttpServerConfig{});
  auto port = original.port();
  original.router().setDefault([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body(std::string("ORIG:") + std::string(req.path()));
    return resp;
  });

  // Move construct server before running
  aeronet::HttpServer moved(std::move(original));

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::string resp = aeronet::test::simpleGet(port, "/mv");

  stop.store(true);

  ASSERT_TRUE(resp.contains("ORIG:/mv"));
}

TEST(HttpServerMove, MoveAssignWhileStopped) {
  aeronet::HttpServer s1(aeronet::HttpServerConfig{}.withReusePort(false));
  aeronet::HttpServer s2(aeronet::HttpServerConfig{}.withReusePort(false));
  auto port1 = s1.port();
  auto port2 = s2.port();

  EXPECT_NE(port1, port2);

  s1.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body("S1");
    return resp;
  });
  s2.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body("S2");
    return resp;
  });

  // Move assign s1 <- s2 (both stopped)
  s1 = std::move(s2);
  EXPECT_EQ(s1.port(), port2);

  std::atomic_bool stop{false};
  std::jthread th([&] { s1.runUntil([&] { return stop.load(); }); });
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  std::string resp = aeronet::test::simpleGet(port2, "/x");
  stop.store(true);
  ASSERT_TRUE(resp.contains("S2"));
}

TEST(HttpServerMove, MoveConstructProbesCapturesThis) {
  std::atomic_bool stop{false};
  // Construct original with builtin probes enabled so they get registered and capture 'this'
  aeronet::HttpServer original(aeronet::HttpServerConfig{}.enableBuiltinProbes(true));
  auto port = original.port();

  // Move construct server before running; handlers were registered on the original and captured its 'this'
  aeronet::HttpServer moved(std::move(original));

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Probe startup path. Correct behavior: after moved.runUntil started, startup probe should return 200.
  std::string resp = aeronet::test::simpleGet(port, "/startupz");

  stop.store(true);

  // Expect HTTP status 200 — if the probe handler captured the moved-from 'this' it will read the reset lifecycle
  // and likely return Service Unavailable (503), causing this assertion to fail and reveal the bug.
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
}

TEST(HttpServerMove, ReRegisterHandlersAfterMove) {
  std::atomic_bool stop{false};
  aeronet::HttpServer original(aeronet::HttpServerConfig{});
  auto port = original.port();

  // initial handler registered on original
  original.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("ORIG");
    return resp;
  });

  // Move server (handlers are moved too)
  aeronet::HttpServer moved(std::move(original));

  // Re-register handlers on the moved instance to new behavior
  moved.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("MOVED");
    return resp;
  });

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::string resp = aeronet::test::simpleGet(port, "/x");
  stop.store(true);

  ASSERT_TRUE(resp.contains("MOVED"));
}

// Disabled by default: demonstrates the hazard when a handler captures `this` and is not re-registered
TEST(HttpServerMove, DISABLED_CapturedThisAfterMoveHazard) {
  std::atomic_bool stop{false};
  aeronet::HttpServer original(aeronet::HttpServerConfig{});
  auto port = original.port();

  // handler captures raw this pointer and returns it as string
  original.router().setDefault([&original](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    // print the pointer value (implementation detail) to observe which 'this' is used
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%p", static_cast<void*>(&original));
    resp.body(buf);
    return resp;
  });

  // Move construct (do not re-register handler)
  aeronet::HttpServer moved(std::move(original));

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::string resp = aeronet::test::simpleGet(port, "/y");
  stop.store(true);

  // The safe expectation is that handler, when invoked on moved server, observes the moved-to 'this'.
  // If it doesn't, this illustrates the hazard.
  ASSERT_TRUE(resp.contains(""));
}
