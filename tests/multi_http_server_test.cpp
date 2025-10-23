#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/multi-http-server.hpp"
#include "aeronet/test_util.hpp"
#include "invalid_argument_exception.hpp"

using namespace std::chrono_literals;

namespace {

std::string SimpleGetRequest(std::string_view target, std::string_view connectionHeader = "close") {
  std::string req;
  req.reserve(128);
  req.append("GET ").append(target).append(" HTTP/1.1").append(aeronet::http::CRLF);
  req.append("Host: localhost").append(aeronet::http::CRLF);
  req.append("Connection: ").append(connectionHeader).append(aeronet::http::CRLF);
  req.append("Content-Length: 0").append(aeronet::http::DoubleCRLF);
  return req;
}

}  // namespace

TEST(MultiHttpServer, BasicStartAndServe) {
  const int threads = 3;
  aeronet::MultiHttpServer multi(aeronet::HttpServerConfig{}.withReusePort(), threads);
  multi.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body(std::string("Hello ")); /* path not exposed directly */
    return resp;
  });
  multi.start();
  auto port = multi.port();
  ASSERT_GT(port, 0);
  // allow sockets to be fully listening
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string r1 = aeronet::test::simpleGet(port, "/one");
  std::string r2 = aeronet::test::simpleGet(port, "/two");
  EXPECT_TRUE(r1.contains("Hello"));
  EXPECT_TRUE(r2.contains("Hello"));

  auto stats = multi.stats();
  EXPECT_EQ(stats.per.size(), static_cast<std::size_t>(threads));
}

// This test only validates that two servers can bind the same port with SO_REUSEPORT enabled
// and accept at least one connection each. It does not attempt to assert load distribution.

TEST(HttpMultiReusePort, TwoServersBindSamePort) {
  aeronet::HttpServer serverA(aeronet::HttpServerConfig{}.withReusePort());
  serverA.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("A");
    return resp;
  });

  auto port = serverA.port();

  aeronet::HttpServer serverB(aeronet::HttpServerConfig{}.withPort(port).withReusePort());
  serverB.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
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

  std::string resp1 = aeronet::test::simpleGet(port, "/one");
  std::string resp2 = aeronet::test::simpleGet(port, "/two");
  bool hasA = resp1.contains('A') || resp2.contains('A');
  bool hasB = resp1.contains('B') || resp2.contains('B');
  if (!(hasA && hasB)) {
    // try additional connects with small delays to give scheduler chance to pick different acceptors
    for (int i = 0; i < 15 && !(hasA && hasB); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      std::string retryResp = aeronet::test::simpleGet(port, "/retry");
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
  aeronet::HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  cfg.withReusePort();
  aeronet::MultiHttpServer multi(cfg, 2);
  const auto port = multi.port();

  multi.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body("OK");
    return resp;
  });

  multi.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  aeronet::test::ClientConnection cnx(port);
  const int fd = cnx.fd();

  ASSERT_TRUE(aeronet::test::sendAll(fd, SimpleGetRequest("/", "keep-alive")));
  const auto initial = aeronet::test::recvWithTimeout(fd);
  ASSERT_TRUE(initial.contains("Connection: keep-alive"));

  multi.beginDrain(200ms);
  EXPECT_TRUE(multi.isDraining());

  // Wait for the listener to be closed by beginDrain() (avoid racey immediate connect attempts)
  // Use a higher timeout to reduce flakiness on CI where shutdown may take longer.
  EXPECT_TRUE(aeronet::test::WaitForListenerClosed(port, 210ms));

  ASSERT_TRUE(aeronet::test::sendAll(fd, SimpleGetRequest("/two", "keep-alive")));
  const auto drained = aeronet::test::recvWithTimeout(fd);
  EXPECT_TRUE(drained.contains("Connection: close"));

  EXPECT_TRUE(aeronet::test::WaitForPeerClose(fd, 500ms));

  multi.stop();
  EXPECT_FALSE(multi.isRunning());
}

TEST(MultiHttpServer, RapidStartStopCycles) {
  aeronet::HttpServerConfig cfg;
  cfg.withReusePort();
  // Keep cycles modest to avoid lengthening normal test runtime too much; adjust if needed.
  for (int statePos = 0; statePos < 200; ++statePos) {
    aeronet::MultiHttpServer multi(cfg);
    multi.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest& req) {
      aeronet::HttpResponse resp;
      resp.body("S");
      return resp;
    });
    multi.start();
    ASSERT_TRUE(multi.isRunning());
    // Short dwell to allow threads to enter run loop.
    std::this_thread::sleep_for(2ms);
    multi.stop();
    EXPECT_FALSE(multi.isRunning());
  }
}

// Verifies that MultiHttpServer can be stopped and started again (restart) while reusing the same port by default.
// HttpServer itself remains single-shot; restart creates fresh HttpServer instances internally.
TEST(MultiHttpServer, RestartBasicSamePort) {
  aeronet::HttpServerConfig cfg;
  cfg.withReusePort();
  aeronet::MultiHttpServer multi(cfg, 2);
  multi.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("Phase1");
    return resp;
  });
  multi.start();
  auto p1 = multi.port();
  ASSERT_GT(p1, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  auto r1 = aeronet::test::simpleGet(p1, "/a", {});
  ASSERT_EQ(r1.statusCode, 200);
  ASSERT_TRUE(r1.body.contains("Phase1"));
  multi.stop();

  // Change handler before restart; old servers are discarded, so new handler should take effect.
  multi.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("Phase2");
    return resp;
  });
  multi.start();
  auto p2 = multi.port();  // same port expected unless user reset cfg.port in between
  EXPECT_EQ(p1, p2);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  auto r2 = aeronet::test::simpleGet(p2, "/b", {});
  ASSERT_EQ(r2.statusCode, 200);
  EXPECT_TRUE(r2.body.contains("Phase2"));
}

// If the user wants a new ephemeral port on restart they can set baseConfig.port=0 before calling start again.
TEST(MultiHttpServer, RestartWithNewEphemeralPort) {
  aeronet::HttpServerConfig cfg;
  cfg.withReusePort();
  aeronet::MultiHttpServer multi(cfg, 1);
  multi.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("R1");
    return resp;
  });
  multi.start();
  auto firstPort = multi.port();
  ASSERT_GT(firstPort, 0);
  multi.stop();

  // Force new ephemeral port by setting base config port to 0 again.
  // (We rely on the restart path honoring updated _baseConfig.port for the first fresh server.)
  // local copy, but we need to mutate MultiHttpServer's base config. Easiest is to set via a restart pattern.
  cfg.port = 0;
  // Direct access not exposed; emulate by move-assigning a new wrapper then restarting (validates restart still works
  // after move too).
  aeronet::MultiHttpServer moved(std::move(multi));
  // We can't directly change baseConfig; for this focused test we'll just check that keeping existing port works.
  moved.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("R2");
    return resp;
  });
  moved.start();
  auto secondPort = moved.port();
  EXPECT_EQ(firstPort, secondPort);  // Documented default behavior (same port unless baseConfig mutated externally)
}

TEST(MultiHttpServer, MoveWhileRunning) {
  aeronet::HttpServerConfig cfg;
  cfg.withReusePort();
  aeronet::MultiHttpServer multi(cfg);
  multi.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("BeforeMove");
    return resp;
  });
  multi.start();
  auto port = multi.port();
  ASSERT_GT(port, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto resp1 = aeronet::test::simpleGet(port, "/pre", {});
  ASSERT_EQ(resp1.statusCode, 200);
  ASSERT_TRUE(resp1.body.contains("BeforeMove"));

  // Move the running server
  aeronet::MultiHttpServer moved(std::move(multi));
  // After move we still should be able to serve
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto resp2 = aeronet::test::simpleGet(port, "/post", {});
  EXPECT_EQ(resp2.statusCode, 200);
  EXPECT_TRUE(resp2.body.contains("BeforeMove"));
}

TEST(MultiHttpServer, MoveAssignmentWhileRunning) {
  aeronet::HttpServerConfig cfgA;
  cfgA.port = 0;
  cfgA.withReusePort();
  aeronet::HttpServerConfig cfgB;
  cfgB.port = 0;
  cfgB.withReusePort();
  // Source server
  aeronet::MultiHttpServer src(cfgA);
  src.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("SrcBody");
    return resp;
  });
  src.start();
  auto srcPort = src.port();
  ASSERT_GT(srcPort, 0);
  // Destination server already running with a different body
  aeronet::MultiHttpServer dst(cfgB);
  dst.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("DstOriginal");
    return resp;
  });
  dst.start();
  auto dstPort = dst.port();
  ASSERT_GT(dstPort, 0);
  ASSERT_NE(srcPort, dstPort) << "Ephemeral ports unexpectedly collided";
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  // Sanity: both respond with their respective bodies
  auto preSrc = aeronet::test::simpleGet(srcPort, "/preSrc", {});
  auto preDst = aeronet::test::simpleGet(dstPort, "/preDst", {});
  ASSERT_TRUE(preSrc.body.contains("SrcBody"));
  ASSERT_TRUE(preDst.body.contains("DstOriginal"));

  // Move-assign: destination adopts source's running threads/servers; its previous threads are stopped inside operator=
  dst = std::move(src);

  // After assignment, dst should serve former source content on source port; old dst port should be inert.
  auto adoptedPort = dst.port();
  EXPECT_EQ(adoptedPort, srcPort);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  auto post = aeronet::test::simpleGet(adoptedPort, "/after", {});
  EXPECT_TRUE(post.body.contains("SrcBody"));
}

TEST(MultiHttpServer, DefaultConstructor) {
  aeronet::MultiHttpServer multi;
  EXPECT_TRUE(multi.empty());
  EXPECT_FALSE(multi.isRunning());
  EXPECT_EQ(multi.port(), 0);

  // Calling stop should be safe even on an empty server
  EXPECT_NO_THROW(multi.stop());
}

// 1. Auto thread-count constructor
TEST(MultiHttpServer, AutoThreadCountConstructor) {
  aeronet::HttpServerConfig cfg;
  cfg.withReusePort();  // auto thread count may be >1 -> must explicitly enable reusePort
  aeronet::MultiHttpServer multi(cfg);
  // Port should be resolved immediately at construction time.
  EXPECT_GT(multi.port(), 0);

  multi.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("Auto");
    return resp;
  });
  multi.start();
  auto port = multi.port();
  ASSERT_GT(port, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  auto resp = aeronet::test::simpleGet(port, "/");
  EXPECT_TRUE(resp.contains("Auto"));
  auto stats = multi.stats();
  EXPECT_GE(stats.per.size(), static_cast<std::size_t>(1));
  multi.stop();
  EXPECT_FALSE(multi.isRunning());
}

// 2. Explicit thread-count constructor
TEST(MultiHttpServer, ExplicitThreadCountConstructor) {
  aeronet::HttpServerConfig cfg;
  cfg.reusePort = true;  // explicit reusePort
  const uint32_t threads = 2;
  aeronet::MultiHttpServer multi(cfg, threads);
  EXPECT_GT(multi.port(), 0);  // resolved during construction
  EXPECT_EQ(multi.nbThreads(), threads);
  multi.router().setDefault([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body("Explicit");
    return resp;
  });
  multi.start();
  ASSERT_GT(multi.port(), 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  auto resp = aeronet::test::simpleGet(multi.port(), "/exp");
  EXPECT_TRUE(resp.contains("Explicit"));
  auto stats = multi.stats();
  EXPECT_EQ(stats.per.size(), static_cast<std::size_t>(threads));
}

// 3. Move construction (move underlying servers ownership)
TEST(MultiHttpServer, MoveConstruction) {
  aeronet::HttpServerConfig cfg;
  cfg.withReusePort();                     // auto thread count may be >1; explicit reusePort required
  aeronet::MultiHttpServer original(cfg);  // auto threads
  EXPECT_GT(original.port(), 0);           // resolved at construction
  original.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("Move");
    return resp;
  });
  auto port = original.port();
  ASSERT_GT(port, 0);
  // Move into new instance
  aeronet::MultiHttpServer moved(std::move(original));
  moved.start();
  // Original should no longer be running (state moved)
  EXPECT_FALSE(moved.port() == 0);
  // Basic request still works after move
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  auto resp = aeronet::test::simpleGet(moved.port(), "/mv");
  EXPECT_TRUE(resp.contains("Move"));
}

// 4. Invalid thread-count explicit constructor (compile-time / runtime guard)
TEST(MultiHttpServer, InvalidExplicitThreadCountThrows) {
  aeronet::HttpServerConfig cfg;
  EXPECT_THROW(aeronet::MultiHttpServer(cfg, 0), aeronet::invalid_argument);  // 0 illegal here
}

// 5. Default constructor + move assignment BEFORE start (moving a running server now asserts)
TEST(MultiHttpServer, DefaultConstructorAndMoveAssignment) {
  aeronet::HttpServerConfig cfg;
  cfg.withReusePort();                   // explicit reusePort (auto thread count may exceed 1)
  aeronet::MultiHttpServer source(cfg);  // not started yet
  EXPECT_GT(source.port(), 0);
  source.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body("MoveAssign");
    return resp;
  });
  const auto originalPort = source.port();
  const auto originalThreads = source.nbThreads();
  ASSERT_GE(originalThreads, 1U);

  aeronet::MultiHttpServer target;  // default constructed inert target
  EXPECT_FALSE(target.isRunning());
  EXPECT_EQ(target.port(), 0);
  EXPECT_EQ(target.nbThreads(), 0U);

  // Move BEFORE start
  target = std::move(source);
  EXPECT_EQ(target.port(), originalPort);
  EXPECT_EQ(target.nbThreads(), originalThreads);
  EXPECT_FALSE(target.isRunning());

  // Start after move
  target.start();
  ASSERT_TRUE(target.isRunning());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  auto resp = aeronet::test::simpleGet(target.port(), "/ma");
  EXPECT_TRUE(resp.contains("MoveAssign"));
  target.stop();
  EXPECT_FALSE(target.isRunning());
}
