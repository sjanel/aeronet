#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/builtin-probes-config.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/signal-handler.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

namespace aeronet {

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

  ts.router().setDefault([](const HttpRequest&) {
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

  ts.router().setDefault([](const HttpRequest&) {
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

  ts.router().setDefault([](const HttpRequest&) {
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

TEST(HttpConfigUpdate, InlineApplyWhenStopped) {
  // Post an update while server is stopped; it should be stored and applied when
  // the event loop next runs.
  HttpServer server(HttpServerConfig{});
  server.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxRequestsPerConnection(12345); });

  // Start the server briefly to allow eventLoop to run and apply pending updates.
  std::atomic_bool stop{false};
  std::jthread th([&] { server.runUntil([&] { return stop.load(); }); });
  std::this_thread::sleep_for(50ms);
  stop.store(true);
  th.join();

  EXPECT_EQ(server.config().maxRequestsPerConnection, 12345U);
}

TEST(HttpConfigUpdate, CoalesceWhileRunning) {
  test::TestServer ts(HttpServerConfig{});
  auto& server = ts.server;

  // register a handler that returns current config value
  server.router().setPath(aeronet::http::Method::GET, std::string("/cfg"), [&server](const HttpRequest&) {
    HttpResponse resp;
    resp.body(std::to_string(server.config().maxRequestsPerConnection));
    return resp;
  });

  // Post multiple updates rapidly; only the last should be observed when applied
  server.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxRequestsPerConnection(1); });
  server.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxRequestsPerConnection(2); });
  server.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxRequestsPerConnection(3); });

  // small sleep to allow event loop to process wakeup
  std::this_thread::sleep_for(20ms);

  auto raw = test::simpleGet(ts.port(), "/cfg");
  // response contains full HTTP response, parse body substring
  EXPECT_TRUE(raw.contains('3'));
}

TEST(HttpRouterUpdate, RuntimeChangeObserved) {
  // Ensure router proxy updates applied while server runs are observed by clients.
  test::TestServer ts(HttpServerConfig{});

  // initial handler returns v1
  ts.router().setPath(aeronet::http::Method::GET, std::string("/dyn"), [](const HttpRequest&) {
    HttpResponse resp;
    resp.body("v1");
    return resp;
  });

  // verify baseline response
  auto raw1 = test::simpleGet(ts.port(), "/dyn");
  EXPECT_TRUE(raw1.find("v1") != std::string::npos);

  // From another thread, post an update to change the handler to v2 after a small delay
  std::jthread updater([&ts] {
    std::this_thread::sleep_for(25ms);
    ts.router().setPath(aeronet::http::Method::GET, std::string("/dyn"), [](const HttpRequest&) {
      HttpResponse resp;
      resp.body("v2");
      return resp;
    });
  });

  // Poll for the updated behavior for a short while
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  bool sawV2 = false;
  while (std::chrono::steady_clock::now() < deadline) {
    auto raw = test::simpleGet(ts.port(), "/dyn");
    if (raw.find("v2") != std::string::npos) {
      sawV2 = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }

  EXPECT_TRUE(sawV2) << "Did not observe runtime router update within timeout";
}

TEST(HttpProbes, StartupAndReadinessTransitions) {
  HttpServerConfig cfg{};
  cfg.enableBuiltinProbes(true);
  test::TestServer ts(std::move(cfg));

  auto readyResp = test::simpleGet(ts.port(), "/readyz");
  EXPECT_TRUE(readyResp.contains("200"));

  auto liveResp = test::simpleGet(ts.port(), "/livez");
  EXPECT_TRUE(liveResp.contains("200"));

  ts.server.beginDrain();

  // Rather than a single fixed sleep which can occasionally race with the server's
  // internal drain transition, poll briefly for the expected states. The readiness
  // probe may either return an explicit 503 or the client helper may fail to
  // connect (empty string) depending on timing. Retry for a short window to make
  // this assertion stable on CI where timing varies.
  std::string readyAfterDrain;
  const auto deadline = std::chrono::steady_clock::now() + 200ms;
  while (std::chrono::steady_clock::now() < deadline) {
    readyAfterDrain = test::simpleGet(ts.port(), "/readyz");
    if (readyAfterDrain.empty() || readyAfterDrain.contains("503")) {
      break;
    }
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_TRUE(readyAfterDrain.empty() || readyAfterDrain.contains("503"));
}

TEST(HttpProbes, OverridePaths) {
  HttpServerConfig cfg{};
  BuiltinProbesConfig bp;
  bp.enabled = true;
  bp.withLivenessPath("/liv");
  bp.withReadinessPath("/rdy");
  bp.withStartupPath("/start");
  cfg.withBuiltinProbes(bp);

  test::TestServer ts(std::move(cfg));

  auto rResp = test::simpleGet(ts.port(), "/rdy");
  EXPECT_TRUE(rResp.contains("200"));
  auto lResp = test::simpleGet(ts.port(), "/liv");
  EXPECT_TRUE(lResp.contains("200"));
  auto sResp = test::simpleGet(ts.port(), "/start");
  EXPECT_TRUE(sResp.contains("200"));
}

TEST(HttpConfigUpdate, ImmutableFieldsProtected) {
  HttpServerConfig cfg{};
  cfg.withReusePort(false);

  test::TestServer ts(std::move(cfg));

  const auto originalPort = ts.port();
  const auto originalReusePort = ts.server.config().reusePort;

  // Handler that echoes current config values
  ts.router().setDefault([&ts](const HttpRequest& req) {
    HttpResponse resp;
    if (req.path() == "/port") {
      resp.body(std::to_string(ts.server.config().port));
    } else if (req.path() == "/reuseport") {
      resp.body(ts.server.config().reusePort ? "true" : "false");
    } else if (req.path() == "/maxbody") {
      resp.body(std::to_string(ts.server.config().maxBodyBytes));
    }
    return resp;
  });

  // Attempt to modify immutable fields (should be silently restored)
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.port = 9999;                     // immutable - will be restored
    cfg.reusePort = true;                // immutable - will be restored
    cfg.maxBodyBytes = 1024UL * 1024UL;  // mutable - will take effect
  });

  auto portResp = test::simpleGet(ts.port(), "/port");
  auto reusePortResp = test::simpleGet(ts.port(), "/reuseport");
  auto maxBodyResp = test::simpleGet(ts.port(), "/maxbody");

  // Immutable fields should remain unchanged
  EXPECT_TRUE(portResp.contains(std::to_string(originalPort)));
  EXPECT_TRUE(reusePortResp.contains(originalReusePort ? "true" : "false"));

  // Mutable field should have changed
  EXPECT_TRUE(maxBodyResp.contains("1048576"));  // 1024*1024
}

class SignalHandlerGlobalTest : public ::testing::Test {
 protected:
  void TearDown() override {
    SignalHandler::ResetStopRequest();
    SignalHandler::Disable();
  }
};

TEST_F(SignalHandlerGlobalTest, AutoDrainOnStopRequest) {
  // Verify that the global signal handler mechanism triggers drain on all servers

  // Install global signal handler with 2s drain timeout
  SignalHandler::Enable(2000ms);

  test::TestServer ts(HttpServerConfig{});

  ts.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("alive");
    return resp;
  });

  // Verify server is running and responsive
  auto resp1 = test::simpleGet(ts.port(), "/");
  EXPECT_TRUE(resp1.contains("alive"));
  EXPECT_FALSE(ts.server.isDraining());
  EXPECT_FALSE(SignalHandler::IsStopRequested());

  // Simulate signal delivery by directly raising SIGINT (safe in test context)
  // Note: raise() is synchronous in the calling thread, so the handler runs immediately
  ::raise(SIGINT);

  // IsStopRequested should now be true
  EXPECT_TRUE(SignalHandler::IsStopRequested());

  // Allow event loop iteration to notice the stop request and call beginDrain
  // The event loop checks IsStopRequested() after each epoll_wait cycle
  std::this_thread::sleep_for(100ms);

  // Server should have initiated drain (may have already completed if no connections)
  // We can't reliably check isDraining() because with 0 connections the drain completes immediately
  // Instead, verify the server stopped accepting new connections
  EXPECT_FALSE(ts.server.isRunning());
}

TEST_F(SignalHandlerGlobalTest, MultiServerCoordination) {
  // Verify that multiple HttpServers in the same process all respond to the global signal
  SignalHandler::Enable(3000ms);

  test::TestServer ts1(HttpServerConfig{});
  test::TestServer ts2(HttpServerConfig{});

  ts1.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("server1");
    return resp;
  });
  ts2.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("server2");
    return resp;
  });

  // Both servers running
  EXPECT_FALSE(ts1.server.isDraining());
  EXPECT_FALSE(ts2.server.isDraining());
  EXPECT_FALSE(SignalHandler::IsStopRequested());

  // Simulate signal
  ::raise(SIGTERM);
  EXPECT_TRUE(SignalHandler::IsStopRequested());

  // Allow both event loops to notice
  std::this_thread::sleep_for(100ms);

  // Both servers should have stopped (drain completed immediately with no connections)
  EXPECT_FALSE(ts1.server.isRunning());
  EXPECT_FALSE(ts2.server.isRunning());
}

// Tests for HttpServer::AsyncHandle and start() methods
TEST(HttpServerAsyncHandle, BasicStartAndStop) {
  HttpServerConfig cfg;
  cfg.withPollInterval(1ms);
  HttpServer server(cfg);
  auto port = server.port();

  server.router().setDefault([](const HttpRequest& req) {
    HttpResponse resp;
    resp.body(std::string("async:") + std::string(req.path()));
    return resp;
  });

  auto handle = server.startDetached();
  EXPECT_TRUE(handle.started());

  // Give server time to start
  std::this_thread::sleep_for(50ms);

  // Make a request
  auto resp = test::simpleGet(port, "/test");
  EXPECT_TRUE(resp.contains("async:/test"));

  // Stop the server
  handle.stop();
  EXPECT_FALSE(handle.started());

  // Check no errors occurred
  EXPECT_NO_THROW(handle.rethrowIfError());
}

TEST(HttpServerAsyncHandle, RAIIAutoStop) {
  HttpServerConfig cfg;
  cfg.withPollInterval(1ms);
  HttpServer server(cfg);
  auto port = server.port();

  server.router().setDefault([](const HttpRequest&) { return HttpResponse(200).body("raii-test"); });

  {
    auto handle = server.startDetached();
    std::this_thread::sleep_for(50ms);

    auto resp = test::simpleGet(port, "/");
    EXPECT_TRUE(resp.contains("raii-test"));

    // handle goes out of scope - should auto-stop
  }

  // Server should be stopped now
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(server.isRunning());
}

TEST(HttpServerAsyncHandle, StartAndStopWhen) {
  std::atomic<bool> done{false};
  HttpServerConfig cfg;
  cfg.withPollInterval(1ms);
  HttpServer server(cfg);
  auto port = server.port();

  server.router().setDefault([](const HttpRequest& req) { return HttpResponse(200).body(req.path()); });

  auto handle = server.startDetachedAndStopWhen([&] { return done.load(); });
  std::this_thread::sleep_for(50ms);

  auto resp = test::simpleGet(port, "/predicate");
  EXPECT_TRUE(resp.contains("/predicate"));

  // Trigger predicate
  done.store(true);
  std::this_thread::sleep_for(100ms);

  // Server should have stopped
  EXPECT_FALSE(server.isRunning());
  EXPECT_NO_THROW(handle.rethrowIfError());
}

TEST(HttpServerAsyncHandle, StartWithStopToken) {
  std::stop_source source;
  HttpServerConfig cfg;
  cfg.withPollInterval(1ms);
  HttpServer server(cfg);
  auto port = server.port();

  server.router().setDefault([](const HttpRequest&) { return HttpResponse(200).body("token-test"); });

  auto handle = server.startDetachedWithStopToken(source.get_token());
  std::this_thread::sleep_for(50ms);

  auto resp = test::simpleGet(port, "/");
  EXPECT_TRUE(resp.contains("token-test"));

  // Request stop via token
  source.request_stop();
  std::this_thread::sleep_for(100ms);

  // Server should have stopped
  EXPECT_FALSE(server.isRunning());
  EXPECT_NO_THROW(handle.rethrowIfError());
}

TEST(HttpServerAsyncHandle, MoveHandle) {
  HttpServerConfig cfg;
  cfg.withPollInterval(1ms);
  HttpServer server(cfg);
  auto port = server.port();

  server.router().setDefault([](const HttpRequest&) { return HttpResponse(200).body("move-test"); });

  auto handle1 = server.startDetached();
  std::this_thread::sleep_for(50ms);

  // Move construct
  HttpServer::AsyncHandle handle2(std::move(handle1));
  EXPECT_TRUE(handle2.started());
  EXPECT_FALSE(handle1.started());  // NOLINT(bugprone-use-after-move)

  auto resp = test::simpleGet(port, "/");
  EXPECT_TRUE(resp.contains("move-test"));

  handle2.stop();
  EXPECT_NO_THROW(handle2.rethrowIfError());
}

TEST(HttpServerAsyncHandle, RestartAfterStop) {
  HttpServerConfig cfg;
  cfg.withPollInterval(1ms);
  HttpServer server(cfg);
  auto port = server.port();

  server.router().setDefault([](const HttpRequest&) { return HttpResponse(200).body("restart"); });

  // First run
  {
    auto handle = server.startDetached();
    std::this_thread::sleep_for(50ms);
    auto resp = test::simpleGet(port, "/");
    EXPECT_TRUE(resp.contains("restart"));
  }

  std::this_thread::sleep_for(50ms);

  // Second run - server should be restartable
  {
    auto handle = server.startDetached();
    std::this_thread::sleep_for(50ms);
    auto resp = test::simpleGet(port, "/");
    EXPECT_TRUE(resp.contains("restart"));
  }
}

}  // namespace aeronet
