#include "aeronet/rate-limit.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

#include "aeronet/timestring.hpp"

namespace aeronet {
namespace {

TEST(RateLimitConfigTest, InvalidValuesThrow) {
  RateLimitConfig cfg;
  cfg.requestsPerSecond = 0;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  cfg = {};
  cfg.burst = 1;
  cfg.requestsPerSecond = 2;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  cfg = {};
  cfg.maxKeys = 0;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(RateLimitStoreTest, AllowsWithinBurstAndRejectsAfter) {
  RateLimitConfig cfg;
  cfg.requestsPerSecond = 2;
  cfg.burst = 2;
  cfg.maxKeys = 64;
  cfg.idleTtl = std::chrono::seconds{10};

  InMemoryTokenBucketRateLimitStore store;
  const auto t0 = std::chrono::steady_clock::time_point{};

  EXPECT_TRUE(store.consume("ip-a", t0, cfg).allowed);
  EXPECT_TRUE(store.consume("ip-a", t0, cfg).allowed);

  const auto denied = store.consume("ip-a", t0, cfg);
  EXPECT_FALSE(denied.allowed);
  EXPECT_GE(denied.retryAfterSeconds, 1U);
}

TEST(RateLimitStoreTest, RefillsOverTime) {
  RateLimitConfig cfg;
  cfg.requestsPerSecond = 2;
  cfg.burst = 2;
  cfg.maxKeys = 64;

  InMemoryTokenBucketRateLimitStore store;
  const auto t0 = std::chrono::steady_clock::time_point{};

  EXPECT_TRUE(store.consume("ip-a", t0, cfg).allowed);
  EXPECT_TRUE(store.consume("ip-a", t0, cfg).allowed);
  EXPECT_FALSE(store.consume("ip-a", t0, cfg).allowed);

  const auto t1 = t0 + std::chrono::milliseconds{500};
  EXPECT_TRUE(store.consume("ip-a", t1, cfg).allowed);

  const auto t2 = t0 + std::chrono::seconds{1};
  EXPECT_TRUE(store.consume("ip-a", t2, cfg).allowed);
}

TEST(RateLimitStoreTest, DifferentKeysAreIndependent) {
  RateLimitConfig cfg;
  cfg.requestsPerSecond = 1;
  cfg.burst = 1;

  InMemoryTokenBucketRateLimitStore store;
  const auto t0 = std::chrono::steady_clock::time_point{};

  EXPECT_TRUE(store.consume("ip-a", t0, cfg).allowed);
  EXPECT_FALSE(store.consume("ip-a", t0, cfg).allowed);
  EXPECT_TRUE(store.consume("ip-b", t0, cfg).allowed);
}

TEST(RateLimitRedisContractTest, BuildsKeyWithHashTagByDefault) {
  RedisSlidingWindowConfig config;
  config.namespacePrefix = "aeronet:rl";
  config.useHashTag = true;
  RedisSlidingWindowRateLimitStore store({}, std::move(config));
  const auto key = store.buildRedisKey("client-1");
  EXPECT_EQ(key, "aeronet:rl:{client-1}");
}

TEST(RateLimitRedisContractTest, BuildsConsumeEvalRequestShape) {
  RedisSlidingWindowRateLimitStore store;
  RateLimitConfig cfg;
  cfg.requestsPerSecond = 5;
  cfg.burst = 10;

  const auto req = store.buildConsumeRequest(
      "client-42", std::chrono::steady_clock::time_point{} + std::chrono::seconds{20000000}, cfg);

  EXPECT_EQ(req.key, "aeronet:rl:{client-42}");
  EXPECT_EQ(req.script, RedisSlidingWindowRateLimitStore::luaSlidingWindowScript());
  EXPECT_EQ(req.scriptSha, RedisSlidingWindowRateLimitStore::luaSlidingWindowScriptSha1());
  EXPECT_EQ(req.args[0], "20000000000");
  EXPECT_EQ(req.args[1], "1000");  // window seconds
  EXPECT_EQ(req.args[2], "5");     // tokens to add (1000ms * 5 rps / 1000ms)
}

TEST(RateLimitRedisContractTest, ParsesDeniedResponse) {
  RateLimitConfig cfg;
  RedisEvalResponse resp;
  resp.ok = true;
  resp.allowed = 0;
  resp.retryAfterSeconds = 3;

  const auto decision = RedisSlidingWindowRateLimitStore::parseConsumeResponse(resp, cfg);
  ASSERT_TRUE(decision.has_value());
  EXPECT_FALSE(decision->allowed);
  EXPECT_EQ(decision->retryAfterSeconds, 3U);
}

}  // namespace
}  // namespace aeronet
