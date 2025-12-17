#include "aeronet/multi-http-server.hpp"

#include <gtest/gtest.h>
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/unix-dogstatsd-sink.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/test-tls-helper.hpp"
#include "aeronet/test_tls_client.hpp"
#endif

using namespace aeronet;
using namespace std::chrono_literals;

namespace {

std::string SimpleGetRequest(std::string_view target, std::string_view connectionHeader = "close") {
  std::string req;
  req.reserve(128);
  req.append("GET ").append(target).append(" HTTP/1.1").append(http::CRLF);
  req.append("Host: localhost").append(http::CRLF);
  req.append("Connection: ").append(connectionHeader).append(http::CRLF);
  req.append("Content-Length: 0").append(http::DoubleCRLF);
  return req;
}

}  // namespace

TEST(MultiHttpServer, ConstructorChecks) {
  EXPECT_NO_THROW(MultiHttpServer(HttpServerConfig{}));
  EXPECT_NO_THROW(MultiHttpServer(HttpServerConfig{}.withReusePort()));
  EXPECT_NO_THROW(MultiHttpServer(HttpServerConfig{}.withReusePort(false)));
}

TEST(MultiHttpServer, EmptyChecks) {
  MultiHttpServer multi;
  EXPECT_TRUE(multi.empty());
  EXPECT_THROW(multi.router(), std::logic_error);
  EXPECT_THROW(multi.run(), std::logic_error);
  EXPECT_THROW(multi.start(), std::logic_error);
  EXPECT_FALSE(multi.isRunning());
  EXPECT_FALSE(multi.isDraining());
  EXPECT_EQ(multi.nbThreads(), 0);

  // Calling stop should be safe even on an empty server
  EXPECT_NO_THROW(multi.stop());

  EXPECT_THROW(multi.postRouterUpdate({}), std::logic_error);
  EXPECT_THROW(multi.postConfigUpdate({}), std::logic_error);

  EXPECT_THROW(multi.setParserErrorCallback({}), std::logic_error);
  EXPECT_THROW(multi.setMetricsCallback({}), std::logic_error);
  EXPECT_THROW(multi.setExpectationHandler({}), std::logic_error);
}

TEST(MultiHttpServer, BasicStartAndServe) {
  const uint32_t threads = 4;
  MultiHttpServer multi(HttpServerConfig{}.withReusePort().withNbThreads(static_cast<uint32_t>(threads)));
  multi.router().setDefault([]([[maybe_unused]] const HttpRequest& req) {
    HttpResponse resp;
    resp.body("Hello "); /* path not exposed directly */
    return resp;
  });
  auto handle = multi.startDetached();

  auto port = multi.port();
  ASSERT_GT(port, 0);

  std::string r1 = test::simpleGet(port, "/one");
  std::string r2 = test::simpleGet(port, "/two");
  EXPECT_TRUE(r1.contains("Hello"));
  EXPECT_TRUE(r2.contains("Hello"));

  auto stats = multi.stats();
  EXPECT_EQ(stats.per.size(), static_cast<std::size_t>(threads));

  EXPECT_THROW((void)multi.startDetached(), std::logic_error);  // already started

  handle.stop();
  handle.rethrowIfError();
}

#ifdef AERONET_ENABLE_OPENSSL
TEST(MultiHttpServer, StatsAggregatesTlsAlpnDistribution) {
  auto [certPem, keyPem] = test::MakeEphemeralCertKey();
  HttpServerConfig cfg;
  cfg.withReusePort();
  cfg.withTlsCertKeyMemory(certPem, keyPem);
  cfg.withTlsAlpnProtocols({"http/1.1"});
  cfg.withNbThreads(1U);
  MultiHttpServer multi(std::move(cfg));
  multi.router().setDefault([]([[maybe_unused]] const HttpRequest&) {
    HttpResponse resp;
    resp.body("TLS");
    return resp;
  });
  auto handle = multi.startDetachedAndStopWhen({});
  test::TlsClient::Options opts;
  opts.alpn = {"http/1.1"};
  test::TlsClient client(multi.port(), opts);
  ASSERT_TRUE(client.handshakeOk());
  auto response = client.get("/alpn");
  EXPECT_TRUE(response.contains("HTTP/1.1 200"));
  auto stats = multi.stats();
  auto it =
      std::ranges::find_if(stats.total.tlsAlpnDistribution, [](const auto& kv) { return kv.first == "http/1.1"; });
  ASSERT_NE(it, stats.total.tlsAlpnDistribution.end());
  EXPECT_GT(it->second, 0U);
  handle.stop();
  handle.rethrowIfError();
}
#endif

// This test only validates that two servers can bind the same port with SO_REUSEPORT enabled
// and accept at least one connection each. It does not attempt to assert load distribution.

TEST(HttpMultiReusePort, TwoServersBindSamePort) {
  SingleHttpServer serverA(HttpServerConfig{}.withReusePort());
  serverA.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("A");
    return resp;
  });

  auto port = serverA.port();

  SingleHttpServer serverB(HttpServerConfig{}.withPort(port).withReusePort());
  serverB.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("B");
    return resp;
  });

  std::promise<void> startedA;
  std::promise<void> startedB;

  std::jthread tA([&] {
    startedA.set_value();
    serverA.run();
  });
  startedA.get_future().wait();
  std::jthread tB([&] {
    startedB.set_value();
    serverB.run();
  });
  startedB.get_future().wait();

  // Give kernel a moment to establish both listening sockets
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::string resp1 = test::simpleGet(port, "/one");
  std::string resp2 = test::simpleGet(port, "/two");
  bool hasA = resp1.contains('A') || resp2.contains('A');
  bool hasB = resp1.contains('B') || resp2.contains('B');
  if (!(hasA && hasB)) {
    // try additional connects with small delays to give scheduler chance to pick different acceptors
    for (int i = 0; i < 15 && !(hasA && hasB); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      std::string retryResp = test::simpleGet(port, "/retry");
      if (retryResp.contains('A')) {
        hasA = true;
      }
      if (retryResp.contains('B')) {
        hasB = true;
      }
    }
  }

  serverA.stop();
  serverB.stop();

  // At least one of the responses should contain body A and one body B
  // Because of hashing, both could come from same server but with two sequential connects
  // we expect distribution eventually, so tolerate the rare case of both identical by allowing either pattern
  EXPECT_TRUE(hasA);
  EXPECT_TRUE(hasB);
}

TEST(MultiHttpServer, BeginDrainClosesKeepAliveConnections) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  cfg.withReusePort();
  cfg.withNbThreads(2U);
  MultiHttpServer multi(std::move(cfg));
  const auto port = multi.port();

  multi.router().setDefault([]([[maybe_unused]] const HttpRequest& req) {
    HttpResponse resp;
    resp.body("OK");
    return resp;
  });

  auto handle = multi.startDetachedAndStopWhen({});

  test::ClientConnection cnx(port);
  const int fd = cnx.fd();

  test::sendAll(fd, SimpleGetRequest("/", "keep-alive"));
  const auto initial = test::recvWithTimeout(fd);
  ASSERT_FALSE(initial.contains("Connection: close"));

  multi.beginDrain(200ms);
  EXPECT_TRUE(multi.isDraining());

  // Wait for the listener to be closed by beginDrain() (avoid racey immediate connect attempts)
  // Use a higher timeout to reduce flakiness on CI where shutdown may take longer.
  EXPECT_TRUE(test::WaitForListenerClosed(port, 210ms));

  test::sendAll(fd, SimpleGetRequest("/two", "keep-alive"));
  const auto drained = test::recvWithTimeout(fd);
  EXPECT_TRUE(drained.contains("Connection: close"));

  EXPECT_TRUE(test::WaitForPeerClose(fd, 500ms));

  handle.stop();
  EXPECT_FALSE(handle.started());
  handle.rethrowIfError();
}

TEST(MultiHttpServer, RapidStartStopCycles) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  // Keep cycles modest to avoid lengthening normal test runtime too much; adjust if needed.
  MultiHttpServer multi(cfg);
  for (int statePos = 0; statePos < 100; ++statePos) {
    multi.router().setDefault([]([[maybe_unused]] const HttpRequest& req) {
      HttpResponse resp;
      resp.body("S");
      return resp;
    });
    auto handle = multi.startDetached();
    ASSERT_TRUE(handle.started());
    // Short dwell to allow threads to enter run loop.
    std::this_thread::sleep_for(2ms);
    handle.stop();
    EXPECT_FALSE(handle.started());
    handle.rethrowIfError();
  }
}

TEST(MultiHttpServer, StartDetachedStopsWhenPredicateFires) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  cfg.withNbThreads(1U);
  MultiHttpServer multi(cfg);
  multi.router().setDefault([]([[maybe_unused]] const HttpRequest&) {
    HttpResponse resp;
    resp.body("Predicate");
    return resp;
  });

  struct PredicateState {
    std::atomic<bool> stop{false};
    std::atomic<bool> observed{false};
    std::atomic<int> invocations{0};
  };

  auto state = std::make_shared<PredicateState>();
  auto handle = multi.startDetachedAndStopWhen([state]() {
    state->invocations.fetch_add(1, std::memory_order_relaxed);
    if (!state->stop.load(std::memory_order_relaxed)) {
      return false;
    }
    state->observed.store(true, std::memory_order_relaxed);
    return true;
  });

  auto port = multi.port();
  ASSERT_GT(port, 0);
  auto resp = test::simpleGet(port, "/predicate");
  EXPECT_TRUE(resp.contains("Predicate"));

  const int nbAttempts = static_cast<int>(cfg.pollInterval / std::chrono::milliseconds(1)) + 1;

  state->stop.store(true, std::memory_order_relaxed);
  for (int attempt = 0; attempt < nbAttempts && !state->observed.load(std::memory_order_relaxed); ++attempt) {
    std::this_thread::sleep_for(1ms);
  }

  EXPECT_TRUE(state->observed.load(std::memory_order_relaxed));
  EXPECT_GT(state->invocations.load(std::memory_order_relaxed), 0);

  handle.stop();
  handle.rethrowIfError();
}

// Verifies that MultiHttpServer can be stopped and started again (restart) while reusing the same port by default.
// SingleHttpServer itself remains single-shot; restart creates fresh SingleHttpServer instances internally.
TEST(MultiHttpServer, RestartBasicSamePort) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  cfg.withNbThreads(2U);
  MultiHttpServer multi(cfg);
  multi.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("Phase1");
    return resp;
  });
  auto handle1 = multi.startDetached();
  auto p1 = multi.port();
  ASSERT_GT(p1, 0);
  auto r1 = test::simpleGet(p1, "/a", {});
  ASSERT_EQ(r1.statusCode, 200);
  ASSERT_TRUE(r1.body.contains("Phase1"));
  handle1.stop();
  handle1.rethrowIfError();

  // Change handler before restart; old servers are discarded, so new handler should take effect.
  multi.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("Phase2");
    return resp;
  });
  auto handle2 = multi.startDetached();
  auto p2 = multi.port();  // same port expected unless user reset cfg.port in between
  EXPECT_EQ(p1, p2);
  auto r2 = test::simpleGet(p2, "/b", {});
  ASSERT_EQ(r2.statusCode, 200);
  EXPECT_TRUE(r2.body.contains("Phase2"));
  handle2.stop();
  handle2.rethrowIfError();
}

TEST(MultiHttpServerCopy, CopyConstructWhileStopped) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  cfg.withNbThreads(2U);
  MultiHttpServer original(cfg);
  original.router().setDefault([]([[maybe_unused]] const HttpRequest&) {
    HttpResponse resp;
    resp.body("COPY-CONST");
    return resp;
  });

  const auto expectedThreads = original.nbThreads();
  const auto expectedPort = original.port();

  MultiHttpServer clone(original);
  original.stop();

  EXPECT_EQ(clone.nbThreads(), expectedThreads);
  EXPECT_EQ(clone.port(), expectedPort);

  auto handle = clone.startDetached();
  auto resp = test::simpleGet(clone.port(), "/copy-construct");
  EXPECT_TRUE(resp.contains("COPY-CONST"));
  handle.stop();
  handle.rethrowIfError();
}

TEST(MultiHttpServerCopy, CopyAssignWhileStopped) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  MultiHttpServer assigned;
  {
    cfg.withNbThreads(2U);
    MultiHttpServer source(cfg);
    source.router().setDefault([]([[maybe_unused]] const HttpRequest&) {
      HttpResponse resp;
      resp.body("COPY-ASSIGN");
      return resp;
    });
    assigned = source;
  }

  EXPECT_FALSE(assigned.empty());
  EXPECT_EQ(assigned.nbThreads(), 2U);

  auto handle = assigned.startDetached();
  auto resp = test::simpleGet(assigned.port(), "/copy-assign");
  EXPECT_TRUE(resp.contains("COPY-ASSIGN"));
  handle.stop();
  handle.rethrowIfError();
}

TEST(MultiHttpServerCopy, CopyConstructWhileRunningThrows) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  cfg.withNbThreads(2U);
  MultiHttpServer original(cfg);
  original.router().setDefault([]([[maybe_unused]] const HttpRequest&) {
    HttpResponse resp;
    resp.body("RUN");
    return resp;
  });

  auto handle = original.startDetached();
  ASSERT_TRUE(handle.started());
  EXPECT_THROW({ (void)MultiHttpServer(original); }, std::logic_error);
  handle.stop();
  handle.rethrowIfError();
}

TEST(MultiHttpServerCopy, CopyAssignWhileRunningThrows) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  cfg.withNbThreads(2U);
  MultiHttpServer target(cfg);
  MultiHttpServer source(cfg);
  source.router().setDefault([]([[maybe_unused]] const HttpRequest&) {
    HttpResponse resp;
    resp.body("RUN");
    return resp;
  });

  auto handle = source.startDetached();
  ASSERT_TRUE(handle.started());
  EXPECT_THROW({ target = source; }, std::logic_error);
  handle.stop();
  handle.rethrowIfError();
}

TEST(MultiHttpServer, MoveThenRestartDifferentConfig) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  cfg.withPollInterval(std::chrono::milliseconds{1});
  cfg.withNbThreads(1U);
  MultiHttpServer multi(cfg);
  multi.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("R1");
    return resp;
  });

  auto port = multi.port();

  auto handle = multi.startDetached();

  static constexpr std::size_t kBodySize = 512;

  auto bodySzStr = std::to_string(kBodySize);

  std::string req = "POST /p HTTP/1.1\r\nConnection: close\r\n";
  req.append("Content-Length: ").append(bodySzStr).append("\r\n\r\n");
  req.append(kBodySize, 'X');

  std::string resp1 = test::sendAndCollect(port, req);

  EXPECT_TRUE(resp1.contains("HTTP/1.1 200"));

  multi.postConfigUpdate([](HttpServerConfig& serverCfg) { serverCfg.maxBodyBytes = kBodySize - 1; });

  std::this_thread::sleep_for(2ms);  // allow config to propagate

  auto firstPort = multi.port();
  ASSERT_GT(firstPort, 0);
  handle.stop();
  EXPECT_FALSE(multi.isRunning());
  handle.rethrowIfError();

  // Direct access not exposed; emulate by move-assigning a new wrapper then restarting (validates restart still works
  // after move too).
  MultiHttpServer moved(std::move(multi));

  // We can't directly change baseConfig; for this focused test we'll just check that keeping existing port works.
  moved.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("R2");
    return resp;
  });
  handle = moved.startDetached();
  auto secondPort = moved.port();

  std::string resp2 = test::sendAndCollect(secondPort, req);

  EXPECT_TRUE(resp2.contains("HTTP/1.1 413 Payload Too Large"));

  EXPECT_EQ(firstPort, secondPort);  // Documented default behavior (same port unless baseConfig mutated externally)
  handle.stop();
  handle.rethrowIfError();
}

TEST(MultiHttpServer, MoveWhileRunning) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  MultiHttpServer multi(cfg);
  multi.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("BeforeMove");
    return resp;
  });
  auto handle = multi.startDetached();
  auto port = multi.port();
  ASSERT_GT(port, 0);
  auto resp1 = test::simpleGet(port, "/pre", {});
  ASSERT_EQ(resp1.statusCode, 200);
  ASSERT_TRUE(resp1.body.contains("BeforeMove"));

  // Move the running server
  MultiHttpServer moved(std::move(multi));
  auto resp2 = test::simpleGet(port, "/post", {});
  EXPECT_EQ(resp2.statusCode, 200);
  EXPECT_TRUE(resp2.body.contains("BeforeMove"));

  handle.stop();
  handle.rethrowIfError();
}

TEST(MultiHttpServer, MoveAssignmentWhileRunning) {
  HttpServerConfig cfgA;
  cfgA.port = 0;
  cfgA.withReusePort();
  HttpServerConfig cfgB;
  cfgB.port = 0;
  cfgB.withReusePort();
  // Source server
  MultiHttpServer src(cfgA);
  src.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("SrcBody");
    return resp;
  });
  auto srcHandle = src.startDetached();
  auto srcPort = src.port();
  ASSERT_GT(srcPort, 0);
  // Destination server already running with a different body
  MultiHttpServer dst(cfgB);
  dst.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("DstOriginal");
    return resp;
  });
  auto dstHandle = dst.startDetached();
  auto dstPort = dst.port();
  ASSERT_GT(dstPort, 0);
  ASSERT_NE(srcPort, dstPort) << "Ephemeral ports unexpectedly collided";
  // Sanity: both respond with their respective bodies
  auto preSrc = test::simpleGet(srcPort, "/preSrc", {});
  auto preDst = test::simpleGet(dstPort, "/preDst", {});
  ASSERT_TRUE(preSrc.body.contains("SrcBody"));
  ASSERT_TRUE(preDst.body.contains("DstOriginal"));

  // Stop both handles before performing any move operations
  // Note: With AsyncHandle pattern, you should stop servers before moving them
  srcHandle.stop();
  dstHandle.stop();

  // Now we can safely move-assign after servers are stopped
  dst = std::move(src);
  EXPECT_EQ(dst.port(), srcPort);

  srcHandle.rethrowIfError();
  dstHandle.rethrowIfError();
}

TEST(MultiHttpServer, AsyncHandleMoveConstructorAndAssignment) {
  HttpServerConfig cfgA;
  cfgA.withReusePort();
  cfgA.withNbThreads(1U);
  MultiHttpServer multiA(cfgA);
  multiA.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("MA");
    return resp;
  });

  // start and obtain a handle
  auto hA = multiA.startDetached();
  ASSERT_TRUE(hA.started());
  std::this_thread::sleep_for(5ms);

  // Move-construct from hA -> hB
  auto hB = std::move(hA);
  EXPECT_TRUE(hB.started());
  EXPECT_FALSE(hA.started());  // NOLINT(bugprone-use-after-move)

  // Start another server to provide a second handle for move-assignment
  HttpServerConfig cfgB;
  cfgB.withReusePort();
  cfgB.withNbThreads(1U);
  MultiHttpServer multiB(cfgB);
  multiB.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("MB");
    return resp;
  });
  auto hC = multiB.startDetached();
  ASSERT_TRUE(hC.started());
  std::this_thread::sleep_for(5ms);

  // Move-assign hB into hC
  hC = std::move(hB);
  // After move assignment, source is moved-from
  EXPECT_FALSE(hB.started());  // NOLINT(bugprone-use-after-move)
  // Destination now should be running
  EXPECT_TRUE(hC.started());

  // Stop and rethrow to ensure clean shutdown
  hC.stop();
  EXPECT_FALSE(hC.started());
  hC.rethrowIfError();
}

TEST(MultiHttpServer, AggregatedStatsJsonAndSetters) {
  // Test AggregatedStats::json_str()
  MultiHttpServer::AggregatedStats stats;
  ServerStats s1;
  s1.totalRequestsServed = 1;
  ServerStats s2;
  s2.totalRequestsServed = 2;
  stats.per.push_back(s1);
  stats.per.push_back(s2);
  auto json = stats.json_str();
  ASSERT_FALSE(json.empty());
  EXPECT_EQ(json.front(), '[');
  EXPECT_EQ(json.back(), ']');
  // Should contain two object markers
  size_t objs = 0;
  for (size_t pos = 0; pos + 1 < json.size(); ++pos) {
    if (json[pos] == '{' && json[pos + 1] == '"') {
      ++objs;
    }
  }
  EXPECT_GE(objs, 2U);

  // Test setters: they should be callable before start() and throw while running
  HttpServerConfig cfg;
  cfg.withReusePort();

  Router router;

  auto& testCbHandler = router.setPath(http::Method::GET, "/test-cb", [](const HttpRequest&) {
    HttpResponse resp;
    resp.body("Cool");
    return resp;
  });

  testCbHandler.after([](const HttpRequest&, HttpResponse& resp) { resp.addHeader("X-After-CB", "Yes"); });

  cfg.withNbThreads(8U);
  MultiHttpServer multi(cfg, std::move(router));

  std::atomic<int> errorsCount{0};
  multi.setParserErrorCallback([&](http::StatusCode) { errorsCount.fetch_add(1, std::memory_order_relaxed); });

  std::atomic<int> metricsCbCount{0};
  multi.setMetricsCallback(
      [&](const SingleHttpServer::RequestMetrics&) { metricsCbCount.fetch_add(1, std::memory_order_relaxed); });

  std::atomic<int> expectCbCount{0};
  multi.setExpectationHandler(
      [&expectCbCount](const HttpRequest& /*req*/, std::string_view /*token*/) -> SingleHttpServer::ExpectationResult {
        expectCbCount.fetch_add(1, std::memory_order_relaxed);
        SingleHttpServer::ExpectationResult expect;
        expect.kind = SingleHttpServer::ExpectationResultKind::Continue;
        return expect;
      });

  std::atomic<int> middlewareCbCount{0};
  multi.setMiddlewareMetricsCallback([&](const SingleHttpServer::MiddlewareMetrics& metrics) {
    EXPECT_EQ(metrics.requestPath, "/test-cb");
    middlewareCbCount.fetch_add(1, std::memory_order_relaxed);
  });

  // start the server briefly
  auto handle = multi.startDetached();

  // Send a normal request to exercise metrics callback
  {
    auto resp = test::simpleGet(multi.port(), "/test-cb");

    EXPECT_TRUE(resp.contains("HTTP/1.1 200"));
    EXPECT_TRUE(resp.contains("X-After-CB: Yes"));
  }

  // Send a malformed request to trigger parser error callback (e.g., invalid start-line)
  {
    test::ClientConnection cnx(multi.port());
    int fd = cnx.fd();
    std::string bad = "BADREQUEST /somepath whatever\r\n\r\n";
    test::sendAll(fd, bad);
    // peer may be closed; just ignore the response
    auto resp = test::recvWithTimeout(fd);
    EXPECT_TRUE(resp.contains("HTTP/1.1 501")) << resp;
  }

  // Validate callbacks were invoked at least once where applicable
  for (int attempts = 0; attempts < 50 && errorsCount.load(std::memory_order_relaxed) == 0; ++attempts) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(errorsCount.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(metricsCbCount.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(middlewareCbCount.load(std::memory_order_relaxed), 1);

  // After start, attempting to set callbacks should throw logic_error
  EXPECT_THROW(multi.setParserErrorCallback({}), std::logic_error);
  EXPECT_THROW(multi.setMetricsCallback({}), std::logic_error);
  EXPECT_THROW(multi.setExpectationHandler({}), std::logic_error);
  EXPECT_THROW(multi.setMiddlewareMetricsCallback({}), std::logic_error);

  handle.stop();
  handle.rethrowIfError();
}

TEST(MultiHttpServer, AutoThreadCountConstructor) {
  HttpServerConfig cfg;
  cfg.withReusePort();  // auto thread count may be >1 -> must explicitly enable reusePort
  MultiHttpServer multi(cfg);
  // Port should be resolved immediately at construction time.
  EXPECT_GT(multi.port(), 0);

  multi.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("Auto");
    return resp;
  });
  auto handle = multi.startDetached();
  auto port = multi.port();
  ASSERT_GT(port, 0);
  auto resp = test::simpleGet(port, "/");
  EXPECT_TRUE(resp.contains("Auto"));
  auto stats = multi.stats();
  EXPECT_GE(stats.per.size(), static_cast<std::size_t>(1));
  handle.stop();
  EXPECT_FALSE(handle.started());
  handle.rethrowIfError();
}

TEST(MultiHttpServer, MoveConstruction) {
  HttpServerConfig cfg;
  MultiHttpServer original(cfg);  // auto threads
  EXPECT_GT(original.port(), 0);  // resolved at construction
  original.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("Move");
    return resp;
  });
  auto port = original.port();
  ASSERT_GT(port, 0);
  // Move into new instance
  MultiHttpServer moved(std::move(original));
  auto handle = moved.startDetached();
  // Original should no longer be running (state moved)
  EXPECT_FALSE(moved.port() == 0);
  // Basic request still works after move
  auto resp = test::simpleGet(moved.port(), "/mv");
  EXPECT_TRUE(resp.contains("Move"));
  handle.stop();
  handle.rethrowIfError();
}

TEST(MultiHttpServer, DefaultConstructorAndMoveAssignment) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  MultiHttpServer source(cfg);  // not started yet
  EXPECT_GT(source.port(), 0);
  source.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("MoveAssign");
    return resp;
  });
  const auto originalPort = source.port();
  const auto originalThreads = source.nbThreads();
  ASSERT_GE(originalThreads, 1U);

  MultiHttpServer target;  // default constructed inert target
  EXPECT_FALSE(target.isRunning());
  EXPECT_EQ(target.nbThreads(), 0U);

  // Move BEFORE start
  target = std::move(source);
  EXPECT_EQ(target.port(), originalPort);
  EXPECT_EQ(target.nbThreads(), originalThreads);
  EXPECT_FALSE(target.isRunning());

  // Start after move
  auto handle = target.startDetached();
  ASSERT_TRUE(handle.started());
  auto resp = test::simpleGet(target.port(), "/ma");
  EXPECT_TRUE(resp.contains("MoveAssign"));
  handle.stop();
  EXPECT_FALSE(handle.started());
  handle.rethrowIfError();
}

TEST(MultiHttpServer, BlockingRunMethod) {
  // Test the blocking run() method which should start servers and block until completion

  HttpServerConfig _cfg;
  _cfg.withReusePort();
  _cfg.withNbThreads(2U);
  MultiHttpServer multi(_cfg);

  multi.router().setDefault([](const HttpRequest& req) {
    HttpResponse resp;
    resp.body(std::string("Blocking:") + std::string(req.path()));
    return resp;
  });

  auto port = multi.port();
  ASSERT_GT(port, 0);

  // Launch run() in a background thread since it blocks
  std::jthread serverThread([&multi]() {
    multi.run();  // This will block until servers complete
  });

  std::this_thread::sleep_for(10ms);  // Give server time to start

  EXPECT_THROW(multi.run(), std::logic_error);  // already running

  // Verify servers are running and responsive
  auto resp1 = test::simpleGet(port, "/test");
  EXPECT_TRUE(resp1.contains("Blocking:/test"));

  // Trigger graceful drain with short timeout to cause run() to complete
  // beginDrain() is safe to call concurrently with run()
  multi.beginDrain(100ms);

  // Wait for run() to complete
  serverThread.join();
}

TEST(MultiHttpServer, RunStopAndRestart) {
  // Test that run() properly cleans up and allows restart

  HttpServerConfig _cfg;
  _cfg.withReusePort().withPollInterval(std::chrono::milliseconds{1});
  _cfg.withNbThreads(2U);
  MultiHttpServer multi(_cfg);

  multi.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("First");
    return resp;
  });

  auto port = multi.port();

  // First run cycle
  auto handle = multi.startDetached();

  EXPECT_TRUE(multi.isRunning());

  auto resp1 = test::simpleGet(port, "/");
  EXPECT_TRUE(resp1.contains("First"));

  // Update handler for second run
  multi.postRouterUpdate([](Router& router) {
    router.setDefault([](const HttpRequest&) {
      HttpResponse resp;
      resp.body("Second");
      return resp;
    });
  });

  std::this_thread::sleep_for(2ms);  // allow update to propagate

  multi.beginDrain(100ms);
  handle.stop();

  EXPECT_FALSE(multi.isRunning());
  handle.rethrowIfError();

  // Second run cycle
  handle = multi.startDetached();

  auto resp2 = test::simpleGet(port, "/");
  EXPECT_FALSE(resp2.contains("First"));
  EXPECT_TRUE(resp2.contains("Second"));

  multi.beginDrain(100ms);
  handle.stop();
  handle.rethrowIfError();
}

TEST(MultiHttpServer, RunUntilStopsWhenPredicateFires) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  cfg.withNbThreads(2U);
  MultiHttpServer multi(cfg);
  multi.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("RunUntil");
    return resp;
  });

  std::atomic<bool> done = false;
  std::jthread runner([&multi, &done](std::stop_token st) {
    multi.runUntil([&done, &st]() { return done.load(std::memory_order_relaxed) || st.stop_requested(); });
  });

  // Wait for the server to be running to avoid race condition where rebuildServers destroys the listener
  while (!multi.isRunning()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  auto port = multi.port();
  ASSERT_GT(port, 0);

  auto resp = test::simpleGet(port, "/run-until");
  EXPECT_TRUE(resp.contains("RunUntil"));

  done.store(true, std::memory_order_relaxed);
  runner.join();

  EXPECT_FALSE(multi.isRunning());
}

TEST(MultiHttpServer, StartDetachedWithStopTokenStopsOnRequest) {
  HttpServerConfig cfg;
  cfg.withReusePort();
  cfg.withNbThreads(1U);
  MultiHttpServer multi(cfg);
  multi.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.body("Token");
    return resp;
  });

  std::stop_source stopSource;
  auto handle = multi.startDetachedWithStopToken(stopSource.get_token());

  auto port = multi.port();
  ASSERT_GT(port, 0);
  auto resp = test::simpleGet(port, "/token");
  EXPECT_TRUE(resp.contains("Token"));

  std::promise<void> initialUpdate;
  multi.postRouterUpdate([&initialUpdate](Router&) { initialUpdate.set_value(); });
  EXPECT_EQ(initialUpdate.get_future().wait_for(200ms), std::future_status::ready);

  stopSource.request_stop();
  std::this_thread::sleep_for(30ms);

  bool stopObserved = false;
  const auto deadline = std::chrono::steady_clock::now() + 200ms;
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      test::simpleGet(port, "/token");
    } catch (...) {
      stopObserved = true;
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_TRUE(stopObserved) << "MultiHttpServer should stop responding once the stop token fires";

  handle.stop();
  handle.rethrowIfError();
}

TEST(MultiHttpServer, ExplicitPortWithNoReusePortShouldCheckPortAvailability) {
  HttpServerConfig cfg;
  cfg.withReusePort(false);

  cfg.withNbThreads(2U);
  MultiHttpServer firstServer(cfg);

  auto port = firstServer.port();
  ASSERT_GT(port, 0);

  cfg.withPort(port);   // set explicit port already in use by firstServer
  cfg.withReusePort();  // enable reusePort for second attempt

  cfg.withNbThreads(2U);
  EXPECT_NO_THROW(MultiHttpServer{cfg});  // should succeed due to reusePort true

  cfg.withReusePort(false);  // disable reusePort again

  // Now, attempt to create another MultiHttpServer on the same port without reusePort
  // -> it should throw due to port being in use by firstServer
  cfg.withNbThreads(2U);
  EXPECT_THROW(MultiHttpServer{cfg}, std::system_error);

  firstServer.stop();
}

TEST(MultiHttpServerTelemetry, MetricsSentViaTelemetryContext) {
  aeronet::test::UnixDogstatsdSink sink;

  TelemetryConfig tcfg;
  tcfg.withDogStatsdSocketPath(sink.path()).withDogStatsdNamespace("svc").enableDogStatsDMetrics(true);

  HttpServerConfig cfg;
  cfg.withTelemetryConfig(std::move(tcfg));

  // Create a MultiHttpServer with one underlying thread so it's valid but simple
  cfg.withNbThreads(1U);
  MultiHttpServer multi(cfg);

  multi.telemetryContext().counterAdd("multi_metric", 1);
  multi.telemetryContext().gauge("multi_gauge", 3);

  EXPECT_EQ(sink.recvMessage(), "svc.multi_metric:1|c");
  EXPECT_EQ(sink.recvMessage(), "svc.multi_gauge:3|g");
}
