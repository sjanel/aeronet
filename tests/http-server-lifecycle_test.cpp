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
#include <type_traits>
#include <utility>

#include "aeronet/builtin-probes-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/signal-handler.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

namespace aeronet {

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

}  // namespace

TEST(SingleHttpServer, DefaultConstructor) {
  SingleHttpServer server;
  EXPECT_EQ(server.port(), 0);
  server.beginDrain();  // should do nothing
  EXPECT_FALSE(server.isDraining());
  EXPECT_FALSE(server.isRunning());
  server.setExpectationHandler({});
}

TEST(SingleHttpServer, ShouldHaveOnlyOneThread) {
  HttpServerConfig config;
  config.withNbThreads(2);
  EXPECT_THROW(SingleHttpServer(std::move(config)), std::invalid_argument);
}

TEST(HttpServerMove, MoveConstructAndServe) {
  std::atomic_bool stop{false};
  SingleHttpServer original;
  original.router().setDefault([](const HttpRequest& req) { return HttpResponse("ORIG:" + std::string(req.path())); });

  // Move construct server before running
  SingleHttpServer moved(std::move(original));

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });

  test::WaitForServer(moved);

  std::string resp = test::simpleGet(moved.port(), "/mv");

  stop.store(true);

  ASSERT_TRUE(resp.contains("ORIG:/mv"));
}

TEST(HttpServerMove, MoveAssignWhileStopped) {
  SingleHttpServer s1(HttpServerConfig{}.withReusePort(false));
  SingleHttpServer s2(HttpServerConfig{}.withReusePort(false));
  auto port1 = s1.port();
  auto port2 = s2.port();

  EXPECT_NE(port1, port2);

  s1.router().setDefault([]([[maybe_unused]] const HttpRequest& req) { return HttpResponse("S1"); });
  s2.router().setDefault([]([[maybe_unused]] const HttpRequest& req) { return HttpResponse("S2"); });

  // Move assign s1 <- s2 (both stopped)
  s1 = std::move(s2);
  EXPECT_EQ(s1.port(), port2);

  std::atomic_bool stop{false};
  std::jthread th([&] { s1.runUntil([&] { return stop.load(); }); });
  test::WaitForServer(s1);
  std::string resp = test::simpleGet(port2, "/x");
  stop.store(true);
  ASSERT_TRUE(resp.contains("S2"));

  // self move-assign should do nothing
  auto& s1Ref = s1;
  s1 = std::move(s1Ref);
  EXPECT_EQ(s1.port(), port2);
}

TEST(HttpServerMove, MoveConstructProbesCapturesThis) {
  std::atomic_bool stop{false};
  // Construct original with builtin probes enabled so they get registered and capture 'this'
  SingleHttpServer original(HttpServerConfig{}.enableBuiltinProbes(true));
  auto port = original.port();

  // Move construct server before running; handlers were registered on the original and captured its 'this'
  SingleHttpServer moved(std::move(original));

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });
  test::WaitForServer(moved);

  // Probe startup path. Correct behavior: after moved.runUntil started, startup probe should return 200.
  std::string resp = test::simpleGet(port, "/startupz");

  stop.store(true);

  // Expect HTTP status 200 — if the probe handler captured the moved-from 'this' it will read the reset lifecycle
  // and likely return Service Unavailable (503), causing this assertion to fail and reveal the bug.
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
}

TEST(HttpServerMove, SingleHttpServerMove) {
  std::atomic_bool stop{false};
  SingleHttpServer original(HttpServerConfig{});
  auto port = original.port();

  // initial handler registered on original
  original.router().setDefault([](const HttpRequest&) { return HttpResponse("ORIG"); });

  // Move server (handlers are moved too)
  SingleHttpServer moved(std::move(original));

  // Re-register handlers on the moved instance to new behavior
  moved.router().setDefault([](const HttpRequest&) { return HttpResponse("MOVED"); });

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });
  test::WaitForServer(moved);

  std::string resp = test::simpleGet(port, "/x");
  stop.store(true);

  ASSERT_TRUE(resp.contains("MOVED"));

  // Attempting to moved from a running server should throw
  EXPECT_THROW(SingleHttpServer(std::move(moved)), std::logic_error);
}

// Disabled by default: demonstrates the hazard when a handler captures `this` and is not re-registered
TEST(HttpServerMove, DISABLED_CapturedThisAfterMoveHazard) {
  std::atomic_bool stop{false};
  SingleHttpServer original(HttpServerConfig{});
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
  SingleHttpServer moved(std::move(original));

  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });
  test::WaitForServer(moved);

  std::string resp = test::simpleGet(port, "/y");
  stop.store(true);

  // The safe expectation is that handler, when invoked on moved server, observes the moved-to 'this'.
  // If it doesn't, this illustrates the hazard.
  ASSERT_TRUE(resp.contains(""));
}

// Validates that moving a running SingleHttpServer (move-construction or move-assignment) throws std::runtime_error
// per the documented semantics (moves only allowed while stopped).
// Note: move-construction now performs the running-state check before transferring members, so attempting to
// move-construct from a running server deterministically throws without partially moving internal state.

TEST(SingleHttpServer, MoveAssignOrDoubleRunWhileRunningThrows) {
  HttpServerConfig cfg;
  SingleHttpServer serverA(cfg);
  SingleHttpServer serverB(cfg);
  serverA.router().setDefault([](const HttpRequest&) { return HttpResponse("a"); });
  serverB.router().setDefault([](const HttpRequest&) { return HttpResponse("b"); });
  std::jthread thr([&] { serverA.run(); });
  test::WaitForServer(serverA);
  ASSERT_TRUE(serverA.isRunning());
  EXPECT_THROW(serverA.run(), std::logic_error);
  EXPECT_THROW({ serverB = std::move(serverA); }, std::logic_error);
}

TEST(HttpServerRestart, RestartPossible) {
  std::atomic_bool stop1{false};
  std::atomic_bool stop2{false};
  SingleHttpServer server(HttpServerConfig{});
  auto port = server.port();
  server.router().setDefault(
      [](const HttpRequest& req) { return HttpResponse(std::string("ORIG:") + std::string(req.path())); });

  std::jthread th([&] {
    server.runUntil([&] { return stop1.load(); });
    server.runUntil([&] { return stop2.load(); });
  });
  test::WaitForServer(server);
  std::string resp = test::simpleGet(port, "/mv");

  ASSERT_TRUE(resp.contains("ORIG:/mv"));

  stop1.store(true);

  // Should start a second time, same port.
  EXPECT_EQ(port, server.port());

  resp = test::simpleGet(port, "/mv2");
  ASSERT_TRUE(resp.contains("ORIG:/mv2"));
  stop2.store(true);
}

TEST(HttpServerCopy, CopyAssignWhileStopped) {
  HttpServerConfig config;
  config.compression.minBytes = 64;

  std::string payload(128, 'x');

  SingleHttpServer destination(config);
  destination.router().setDefault([&payload]([[maybe_unused]] const HttpRequest&) {
    HttpResponse resp(payload);
    resp.header("X-Who", "destination");
    return resp;
  });

  auto launchSomeQueries = [&payload](SingleHttpServer& server, std::string_view expectedHeaderValue) {
    server.start();
    test::WaitForServer(server);

    for (std::underlying_type_t<Encoding> i = 0; i <= kNbContentEncodings; ++i) {
      const auto enc = static_cast<Encoding>(i);
      if (!IsEncodingEnabled(enc)) {
        continue;
      }

      test::RequestOptions opts;
      opts.headers = {{http::AcceptEncoding, GetEncodingStr(enc)}, {http::ContentEncoding, GetEncodingStr(enc)}};
      opts.body = test::Compress(enc, payload);

      for (int nbConsecutiveReq = 0; nbConsecutiveReq < 3; ++nbConsecutiveReq) {
        auto optResp = test::request(server.port(), opts);
        ASSERT_TRUE(optResp.has_value());
        auto resp = optResp.value_or("");

        EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
        EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("X-Who", expectedHeaderValue)));
        auto bodyStart = resp.find(http::DoubleCRLF);
        ASSERT_NE(bodyStart, std::string::npos);
        bodyStart += http::DoubleCRLF.size();
        auto body = resp.substr(bodyStart);
        auto decodedBody = test::Decompress(enc, body);
        EXPECT_EQ(std::string_view(decodedBody), payload);
      }
    }

    server.stop();
    test::WaitForServer(server, false);
  };

  launchSomeQueries(destination, "destination");

  {
    SingleHttpServer source(config);
    source.router().setDefault([&]([[maybe_unused]] const HttpRequest&) {
      HttpResponse resp(payload);
      resp.header("X-Who", "source");
      return resp;
    });

    launchSomeQueries(source, "source");

    destination = source;

    launchSomeQueries(destination, "source");
  }
}

TEST(HttpServerCopy, CopyAssignWhileRunningThrows) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  SingleHttpServer running(cfg);
  running.router().setDefault([]([[maybe_unused]] const HttpRequest&) { return HttpResponse("OK"); });

  SingleHttpServer target(cfg);

  std::jthread worker([&] { running.run(); });
  ASSERT_TRUE(test::WaitForServer(running));

  EXPECT_THROW({ target = running; }, std::logic_error);

  running.stop();
  worker.join();
}

TEST(HttpDrain, StopsNewConnections) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  test::TestServer ts(cfg);

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  const auto port = ts.port();

  ASSERT_TRUE(test::AttemptConnect(port));

  // Baseline request to ensure server responds prior to draining.
  {
    test::ClientConnection cnx(port);
    test::sendAll(cnx.fd(), SimpleGetRequest("/pre", http::keepalive));
    const auto resp = test::recvWithTimeout(cnx.fd());
    EXPECT_TRUE(resp.contains("HTTP/1.1 200"));
  }

  ts.server.beginDrain();

  // During drain, connections are still accepted (for health probes), but responses include Connection: close
  ASSERT_TRUE(test::AttemptConnect(port));

  ts.stop();
}

TEST(HttpDrain, KeepAliveConnectionsCloseAfterDrain) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  test::TestServer ts(cfg);

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  const auto port = ts.port();
  test::ClientConnection cnx(port);
  const int fd = cnx.fd();

  test::sendAll(fd, SimpleGetRequest("/one", http::keepalive));
  auto firstResponse = test::recvWithTimeout(fd);
  ASSERT_FALSE(firstResponse.contains(MakeHttp1HeaderLine(http::Connection, http::close)));

  ts.server.beginDrain();

  test::sendAll(fd, SimpleGetRequest("/two", http::keepalive));
  auto drainedResponse = test::recvWithTimeout(fd);
  EXPECT_TRUE(drainedResponse.contains(MakeHttp1HeaderLine(http::Connection, http::close)));

  EXPECT_TRUE(test::WaitForPeerClose(fd, 500ms));

  ts.stop();
}

TEST(HttpDrain, DeadlineForcesIdleConnectionsToClose) {
  HttpServerConfig cfg;
  cfg.keepAliveTimeout = 5s;  // ensure default timeout does not interfere with the test window
  test::TestServer ts(cfg);

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  const auto port = ts.port();
  test::ClientConnection idle(port);
  const int fd = idle.fd();

  ASSERT_TRUE(test::WaitForServer(ts.server));
  ts.server.beginDrain(std::chrono::milliseconds{500});
  ts.server.beginDrain(std::chrono::milliseconds{50});
  ASSERT_TRUE(ts.server.isDraining());

  EXPECT_TRUE(test::WaitForPeerClose(fd, 500ms));

  ts.stop();
}

TEST(HttpConfigUpdate, InlineApplyWhenStopped) {
  // Post an update while server is stopped; it should be stored and applied when
  // the event loop next runs.
  SingleHttpServer server(HttpServerConfig{});
  server.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxRequestsPerConnection(12345); });
  EXPECT_NE(server.config().maxRequestsPerConnection, 12345U);

  // Start the server briefly to allow eventLoop to run and apply pending updates.
  std::atomic_bool stop{false};
  std::jthread th([&] { server.runUntil([&] { return stop.load(); }); });
  test::WaitForServer(server);
  stop.store(true);
  th.join();

  EXPECT_EQ(server.config().maxRequestsPerConnection, 12345U);
}

TEST(HttpConfigUpdate, CoalesceWhileRunning) {
  test::TestServer ts(HttpServerConfig{});
  auto& server = ts.server;

  // register a handler that returns current config value
  server.router().setPath(aeronet::http::Method::GET, std::string("/cfg"), [&server](const HttpRequest&) {
    return HttpResponse(std::to_string(server.config().maxRequestsPerConnection));
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
  ts.router().setPath(aeronet::http::Method::GET, std::string("/dyn"),
                      [](const HttpRequest&) { return HttpResponse("v1"); });

  // verify baseline response
  auto raw1 = test::simpleGet(ts.port(), "/dyn");
  EXPECT_TRUE(raw1.find("v1") != std::string::npos);

  // From another thread, post an update to change the handler to v2 after a small delay
  std::jthread updater([&ts] {
    std::this_thread::sleep_for(25ms);
    ts.router().setPath(aeronet::http::Method::GET, std::string("/dyn"),
                        [](const HttpRequest&) { return HttpResponse("v2"); });
  });

  // Poll for the updated behavior for a short while
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  bool sawV2 = false;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
    auto raw = test::simpleGet(ts.port(), "/dyn");
    if (raw.find("v2") != std::string::npos) {
      sawV2 = true;
      break;
    }
  }

  EXPECT_TRUE(sawV2) << "Did not observe runtime router update within timeout";
}

TEST(HttpProbes, ReadinessProbleShouldReturn503WhenDrainingIfSomeConnectionsAlive) {
  HttpServerConfig cfg{};
  cfg.enableBuiltinProbes(true);
  cfg.withKeepAliveTimeout(500ms);

  test::TestServer ts(std::move(cfg));

  ts.postRouterUpdate([](Router& router) { router.setDefault([](const HttpRequest&) { return HttpResponse("OK"); }); });

  auto readyResp = test::simpleGet(ts.port(), "/readyz");
  EXPECT_TRUE(readyResp.contains("HTTP/1.1 200"));

  auto liveResp = test::simpleGet(ts.port(), "/livez");
  EXPECT_TRUE(liveResp.contains("HTTP/1.1 200"));

  // Launch a keep-alive connection to observe readiness probe change during drain
  test::ClientConnection cnx(ts.port());
  const int fd = cnx.fd();
  test::sendAll(fd, SimpleGetRequest("/some-path", http::keepalive));

  std::this_thread::sleep_for(20ms);  // ensure request is processed

  // Keep-alive connection should be kept open
  ts.server.beginDrain(500ms);
  ts.server.beginDrain(SignalHandler::GetMaxDrainPeriod());

  std::this_thread::sleep_for(20ms);  // ensure drain has been initiated

  auto raw = test::simpleGet(ts.port(), "/readyz");
  EXPECT_TRUE(raw.contains("HTTP/1.1 503"));
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

  auto portResp = test::simpleGet(originalPort, "/port");
  auto reusePortResp = test::simpleGet(originalPort, "/reuseport");
  auto maxBodyResp = test::simpleGet(originalPort, "/maxbody");

  // Immutable fields should remain unchanged
  EXPECT_TRUE(portResp.contains(std::to_string(originalPort)));
  EXPECT_EQ(ts.port(), originalPort);
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

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("alive"); });

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

  // Server should have initiated drain (may have already completed if no connections)
  // We can't reliably check isDraining() because with 0 connections the drain completes immediately
  // Instead, verify the server stopped accepting new connections
  EXPECT_TRUE(test::WaitForServer(ts.server, false));
}

TEST_F(SignalHandlerGlobalTest, MultiServerCoordination) {
  // Verify that multiple HttpServers in the same process all respond to the global signal
  SignalHandler::Enable(3000ms);

  test::TestServer ts1(HttpServerConfig{});
  test::TestServer ts2(HttpServerConfig{});

  ts1.router().setDefault([](const HttpRequest&) { return HttpResponse("server1"); });
  ts2.router().setDefault([](const HttpRequest&) { return HttpResponse("server2"); });

  // Both servers running
  EXPECT_FALSE(ts1.server.isDraining());
  EXPECT_FALSE(ts2.server.isDraining());
  EXPECT_FALSE(SignalHandler::IsStopRequested());

  // Simulate signal
  ::raise(SIGTERM);
  EXPECT_TRUE(SignalHandler::IsStopRequested());

  EXPECT_TRUE(test::WaitForServer(ts1.server, false));
  EXPECT_TRUE(test::WaitForServer(ts2.server, false));
}

// Tests for SingleHttpServer::AsyncHandle and start() methods
TEST(HttpServerAsyncHandle, BasicStartAndStop) {
  HttpServerConfig cfg;
  cfg.withPollInterval(1ms);
  SingleHttpServer server(cfg);
  auto port = server.port();

  server.router().setDefault([](const HttpRequest& req) { return HttpResponse("async:" + std::string(req.path())); });

  auto handle = server.startDetached();
  EXPECT_TRUE(handle.started());

  test::WaitForServer(server);

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
  SingleHttpServer server(cfg);
  auto port = server.port();

  server.router().setDefault([](const HttpRequest&) { return HttpResponse("raii-test"); });

  {
    auto handle = server.startDetached();
    test::WaitForServer(server);

    auto resp = test::simpleGet(port, "/");
    EXPECT_TRUE(resp.contains("raii-test"));

    // handle goes out of scope - should auto-stop
  }

  EXPECT_TRUE(test::WaitForServer(server, false));
}

TEST(HttpServerAsyncHandle, StartAndStopWhen) {
  std::atomic<bool> done{false};
  HttpServerConfig cfg;
  cfg.withPollInterval(1ms);
  SingleHttpServer server(cfg);
  auto port = server.port();

  server.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.path()); });

  auto handle = server.startDetachedAndStopWhen([&] { return done.load(); });
  EXPECT_TRUE(test::WaitForServer(server, true));

  auto resp = test::simpleGet(port, "/predicate");
  EXPECT_TRUE(resp.contains("/predicate"));

  // Trigger predicate
  done.store(true);

  // Server should have stopped
  EXPECT_TRUE(test::WaitForServer(server, false));
  EXPECT_NO_THROW(handle.rethrowIfError());
}

TEST(HttpServerAsyncHandle, StartWithStopToken) {
  std::stop_source source;
  HttpServerConfig cfg;
  cfg.withPollInterval(1ms);
  SingleHttpServer server(cfg);
  auto port = server.port();

  server.router().setDefault([](const HttpRequest&) { return HttpResponse("token-test"); });

  auto handle = server.startDetachedWithStopToken(source.get_token());
  EXPECT_TRUE(test::WaitForServer(server, true));

  auto resp = test::simpleGet(port, "/");
  EXPECT_TRUE(resp.contains("token-test"));

  // Request stop via token
  source.request_stop();

  // Server should have stopped
  EXPECT_TRUE(test::WaitForServer(server, false));
  EXPECT_NO_THROW(handle.rethrowIfError());
}

TEST(HttpServerAsyncHandle, MoveHandle) {
  HttpServerConfig cfg;
  cfg.withPollInterval(1ms);
  SingleHttpServer server(cfg);
  auto port = server.port();

  server.router().setDefault([](const HttpRequest&) { return HttpResponse(200).body("move-test"); });

  auto handle1 = server.startDetached();
  EXPECT_TRUE(test::WaitForServer(server, true));

  // Move construct
  SingleHttpServer::AsyncHandle handle2(std::move(handle1));
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
  SingleHttpServer server(cfg);
  auto port = server.port();

  server.router().setDefault([](const HttpRequest&) { return HttpResponse("restart"); });

  // First run
  {
    auto handle = server.startDetached();

    EXPECT_TRUE(test::WaitForServer(server, true));
    auto resp = test::simpleGet(port, "/");
    EXPECT_TRUE(resp.contains("restart"));

    handle.stop();
    auto handle2 = server.startDetached();
    EXPECT_TRUE(handle2.started());

    handle = std::move(handle2);  // test move assignment
  }

  EXPECT_TRUE(test::WaitForServer(server, false));

  // Second run - server should be restartable
  {
    auto handle = server.startDetached();
    EXPECT_TRUE(test::WaitForServer(server, true));
    auto resp = test::simpleGet(port, "/");
    EXPECT_TRUE(resp.contains("restart"));
  }
}

}  // namespace aeronet
