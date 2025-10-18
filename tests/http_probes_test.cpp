#include <gtest/gtest.h>

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

  std::this_thread::sleep_for(20ms);

  auto readyResp = aeronet::test::simpleGet(ts.port(), "/readyz");
  EXPECT_TRUE(readyResp.contains("200"));

  auto liveResp = aeronet::test::simpleGet(ts.port(), "/livez");
  EXPECT_TRUE(liveResp.contains("200"));

  ts.server.beginDrain();
  std::this_thread::sleep_for(5ms);
  auto readyAfterDrain = aeronet::test::simpleGet(ts.port(), "/readyz");
  // The server marks readiness=false before closing the listener. Depending on timing the
  // listening socket may be closed before the probe request reaches the server which
  // results in an empty response (connection failed) from the client helper. Accept
  // either an explicit 503 Service Unavailable response or an empty string here.
  EXPECT_TRUE(readyAfterDrain.empty() || readyAfterDrain.contains("503"));
}

TEST(HttpProbes, OverridePaths) {
  HttpServerConfig cfg{};
  BuiltinProbesConfig bp;
  bp.enabled = true;
  bp.readinessPath = "/rdy";
  bp.livenessPath = "/liv";
  bp.startupPath = "/start";
  cfg.withBuiltinProbes(bp);

  aeronet::test::TestServer ts(std::move(cfg));
  std::this_thread::sleep_for(20ms);

  auto rResp = aeronet::test::simpleGet(ts.port(), "/rdy");
  EXPECT_TRUE(rResp.contains("200"));
  auto lResp = aeronet::test::simpleGet(ts.port(), "/liv");
  EXPECT_TRUE(lResp.contains("200"));
  auto sResp = aeronet::test::simpleGet(ts.port(), "/start");
  EXPECT_TRUE(sResp.contains("200"));
}
