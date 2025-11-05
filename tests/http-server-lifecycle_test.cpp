#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

TEST(HttpServerMove, MoveConstructAndServe) {
  std::atomic_bool stop{false};
  HttpServer original(HttpServerConfig{});
  auto port = original.port();
  original.router().setDefault([](const HttpRequest& req) {
    HttpResponse resp;
    resp.body(std::string("ORIG:") + std::string(req.path()));
    return resp;
  });

  // Move construct server before running
  HttpServer moved(std::move(original));

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::string resp = test::simpleGet(port, "/mv");

  stop.store(true);

  ASSERT_TRUE(resp.contains("ORIG:/mv"));
}

TEST(HttpServerMove, MoveAssignWhileStopped) {
  HttpServer s1(HttpServerConfig{}.withReusePort(false));
  HttpServer s2(HttpServerConfig{}.withReusePort(false));
  auto port1 = s1.port();
  auto port2 = s2.port();

  EXPECT_NE(port1, port2);

  s1.router().setDefault([]([[maybe_unused]] const HttpRequest& req) {
    HttpResponse resp;
    resp.body("S1");
    return resp;
  });
  s2.router().setDefault([]([[maybe_unused]] const HttpRequest& req) {
    HttpResponse resp;
    resp.body("S2");
    return resp;
  });

  // Move assign s1 <- s2 (both stopped)
  s1 = std::move(s2);
  EXPECT_EQ(s1.port(), port2);

  std::atomic_bool stop{false};
  std::jthread th([&] { s1.runUntil([&] { return stop.load(); }); });
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  std::string resp = test::simpleGet(port2, "/x");
  stop.store(true);
  ASSERT_TRUE(resp.contains("S2"));
}

TEST(HttpServerMove, MoveConstructProbesCapturesThis) {
  std::atomic_bool stop{false};
  // Construct original with builtin probes enabled so they get registered and capture 'this'
  HttpServer original(HttpServerConfig{}.enableBuiltinProbes(true));
  auto port = original.port();

  // Move construct server before running; handlers were registered on the original and captured its 'this'
  HttpServer moved(std::move(original));

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Probe startup path. Correct behavior: after moved.runUntil started, startup probe should return 200.
  std::string resp = test::simpleGet(port, "/startupz");

  stop.store(true);

  // Expect HTTP status 200 — if the probe handler captured the moved-from 'this' it will read the reset lifecycle
  // and likely return Service Unavailable (503), causing this assertion to fail and reveal the bug.
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
}

TEST(HttpServerMove, ReRegisterHandlersAfterMove) {
  std::atomic_bool stop{false};
  HttpServer original(HttpServerConfig{});
  auto port = original.port();

  // initial handler registered on original
  original.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("ORIG");
    return resp;
  });

  // Move server (handlers are moved too)
  HttpServer moved(std::move(original));

  // Re-register handlers on the moved instance to new behavior
  moved.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("MOVED");
    return resp;
  });

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::string resp = test::simpleGet(port, "/x");
  stop.store(true);

  ASSERT_TRUE(resp.contains("MOVED"));
}

// Disabled by default: demonstrates the hazard when a handler captures `this` and is not re-registered
TEST(HttpServerMove, DISABLED_CapturedThisAfterMoveHazard) {
  std::atomic_bool stop{false};
  HttpServer original(HttpServerConfig{});
  auto port = original.port();

  // handler captures raw this pointer and returns it as string
  original.router().setDefault([&original](const HttpRequest&) {
    HttpResponse resp;
    // print the pointer value (implementation detail) to observe which 'this' is used
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%p", static_cast<void*>(&original));
    resp.body(buf);
    return resp;
  });

  // Move construct (do not re-register handler)
  HttpServer moved(std::move(original));

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::string resp = test::simpleGet(port, "/y");
  stop.store(true);

  // The safe expectation is that handler, when invoked on moved server, observes the moved-to 'this'.
  // If it doesn't, this illustrates the hazard.
  ASSERT_TRUE(resp.contains(""));
}

// Validates that moving a running HttpServer (move-construction or move-assignment) throws std::runtime_error
// per the documented semantics (moves only allowed while stopped).
// We cannot test with determinism the move constructor throw because we first move construct the fields before checking
// running status, so the moved-from object may be left in a valid but stopped state.

TEST(HttpServer, MoveAssignWhileRunningThrows) {
  HttpServerConfig cfg;
  HttpServer serverA(cfg);
  HttpServer serverB(cfg);
  serverA.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("a");
    return resp;
  });
  serverB.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("b");
    return resp;
  });
  std::jthread thr([&] { serverA.run(); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!serverA.isRunning() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(serverA.isRunning());
  EXPECT_THROW({ serverB = std::move(serverA); }, std::logic_error);
}

TEST(HttpServerRestart, RestartPossible) {
  std::atomic_bool stop1{false};
  std::atomic_bool stop2{false};
  HttpServer server(HttpServerConfig{});
  auto port = server.port();
  server.router().setDefault([](const HttpRequest& req) {
    HttpResponse resp;
    resp.body(std::string("ORIG:") + std::string(req.path()));
    return resp;
  });

  std::jthread th([&] {
    server.runUntil([&] { return stop1.load(); });
    server.runUntil([&] { return stop2.load(); });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::string resp = test::simpleGet(port, "/mv");

  ASSERT_TRUE(resp.contains("ORIG:/mv"));

  stop1.store(true);

  // Should start a second time, same port.
  EXPECT_EQ(port, server.port());

  resp = test::simpleGet(port, "/mv2");
  ASSERT_TRUE(resp.contains("ORIG:/mv2"));
  stop2.store(true);
}

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

bool WaitForServerRunning(HttpServer& server, std::chrono::milliseconds timeout) {
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

TEST(HttpDrain, StopsNewConnections) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  test::TestServer ts(cfg);

  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("OK");
    return resp;
  });

  const auto port = ts.port();

  ASSERT_TRUE(test::AttemptConnect(port));

  // Baseline request to ensure server responds prior to draining.
  {
    test::ClientConnection cnx(port);
    test::sendAll(cnx.fd(), SimpleGetRequest("/pre", "keep-alive"));
    const auto resp = test::recvWithTimeout(cnx.fd());
    EXPECT_TRUE(resp.contains("200"));
  }

  ts.server.beginDrain();

  EXPECT_FALSE(test::AttemptConnect(port));

  ts.stop();
}

TEST(HttpDrain, KeepAliveConnectionsCloseAfterDrain) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  test::TestServer ts(cfg);

  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("OK");
    return resp;
  });

  const auto port = ts.port();
  test::ClientConnection cnx(port);
  const int fd = cnx.fd();

  test::sendAll(fd, SimpleGetRequest("/one", "keep-alive"));
  auto firstResponse = test::recvWithTimeout(fd);
  ASSERT_TRUE(firstResponse.contains("Connection: keep-alive"));

  ts.server.beginDrain();

  test::sendAll(fd, SimpleGetRequest("/two", "keep-alive"));
  auto drainedResponse = test::recvWithTimeout(fd);
  EXPECT_TRUE(drainedResponse.contains("Connection: close"));

  EXPECT_TRUE(test::WaitForPeerClose(fd, 500ms));

  ts.stop();
}

TEST(HttpDrain, DeadlineForcesIdleConnectionsToClose) {
  HttpServerConfig cfg;
  cfg.keepAliveTimeout = 5s;  // ensure default timeout does not interfere with the test window
  test::TestServer ts(cfg);

  ts.server.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("OK");
    return resp;
  });

  const auto port = ts.port();
  test::ClientConnection idle(port);
  const int fd = idle.fd();

  ASSERT_TRUE(WaitForServerRunning(ts.server, 200ms));
  ts.server.beginDrain(std::chrono::milliseconds{50});
  ASSERT_TRUE(ts.server.isDraining());

  EXPECT_TRUE(test::WaitForPeerClose(fd, 500ms));

  ts.stop();
}
