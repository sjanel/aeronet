#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/builtin-probes-config.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;
using namespace std::chrono_literals;

TEST(HttpProbes, StartupAndReadinessTransitions) {
  HttpServerConfig cfg{};
  cfg.enableBuiltinProbes(true);
  aeronet::test::TestServer ts(std::move(cfg));

  auto readyResp = aeronet::test::simpleGet(ts.port(), "/readyz");
  EXPECT_TRUE(readyResp.contains("200"));

  auto liveResp = aeronet::test::simpleGet(ts.port(), "/livez");
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
    readyAfterDrain = aeronet::test::simpleGet(ts.port(), "/readyz");
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

  aeronet::test::TestServer ts(std::move(cfg));

  auto rResp = aeronet::test::simpleGet(ts.port(), "/rdy");
  EXPECT_TRUE(rResp.contains("200"));
  auto lResp = aeronet::test::simpleGet(ts.port(), "/liv");
  EXPECT_TRUE(lResp.contains("200"));
  auto sResp = aeronet::test::simpleGet(ts.port(), "/start");
  EXPECT_TRUE(sResp.contains("200"));
}
