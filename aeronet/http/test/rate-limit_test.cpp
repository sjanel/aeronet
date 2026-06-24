#include "aeronet/rate-limit.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

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

  cfg = {};
  cfg.idleTtl = std::chrono::seconds{0};
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  cfg = {};
  EXPECT_NO_THROW(cfg.validate());
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

TEST(RateLimitStoreTest, EmptyKeyBypassesLimits) {
  RateLimitConfig cfg;
  cfg.requestsPerSecond = 1;
  cfg.burst = 1;
  cfg.maxKeys = 0;

  InMemoryTokenBucketRateLimitStore store;
  const auto t0 = std::chrono::steady_clock::time_point{};

  EXPECT_TRUE(store.consume({}, t0, cfg).allowed);
}

TEST(RateLimitStoreTest, MaxKeysZeroEvictionPathKeepsNewKeyUsable) {
  RateLimitConfig cfg;
  cfg.requestsPerSecond = 1;
  cfg.burst = 1;
  cfg.maxKeys = 0;

  InMemoryTokenBucketRateLimitStore store;
  const auto t0 = std::chrono::steady_clock::time_point{};

  EXPECT_TRUE(store.consume("ip-a", t0, cfg).allowed);
  EXPECT_FALSE(store.consume("ip-a", t0, cfg).allowed);
}

TEST(RateLimitStoreTest, EvictsIdleKeyWhenCapacityReached) {
  RateLimitConfig cfg;
  cfg.requestsPerSecond = 1;
  cfg.burst = 1;
  cfg.maxKeys = 1;
  cfg.idleTtl = std::chrono::seconds{1};

  InMemoryTokenBucketRateLimitStore store;
  const auto t0 = std::chrono::steady_clock::time_point{};

  EXPECT_TRUE(store.consume("ip-old", t0, cfg).allowed);
  EXPECT_TRUE(store.consume("ip-new", t0 + std::chrono::seconds{1}, cfg).allowed);
  EXPECT_FALSE(store.consume("ip-new", t0 + std::chrono::seconds{1}, cfg).allowed);
}

TEST(RateLimitStoreTest, EvictsFirstKeyWhenNoIdleKeyExists) {
  RateLimitConfig cfg;
  cfg.requestsPerSecond = 1;
  cfg.burst = 1;
  cfg.maxKeys = 1;
  cfg.idleTtl = std::chrono::seconds{10};

  InMemoryTokenBucketRateLimitStore store;
  const auto t0 = std::chrono::steady_clock::time_point{};

  EXPECT_TRUE(store.consume("ip-a", t0, cfg).allowed);
  EXPECT_TRUE(store.consume("ip-b", t0, cfg).allowed);
  EXPECT_FALSE(store.consume("ip-b", t0, cfg).allowed);
}

TEST(RateLimitStoreTest, EvictsMultipleIdleKeysAfterLimitIsLowered) {
  RateLimitConfig fillCfg;
  fillCfg.requestsPerSecond = 1;
  fillCfg.burst = 1;
  fillCfg.maxKeys = 8;
  fillCfg.idleTtl = std::chrono::seconds{1};

  InMemoryTokenBucketRateLimitStore store;
  const auto t0 = std::chrono::steady_clock::time_point{};

  EXPECT_TRUE(store.consume("ip-a", t0, fillCfg).allowed);
  EXPECT_TRUE(store.consume("ip-b", t0, fillCfg).allowed);
  EXPECT_TRUE(store.consume("ip-c", t0, fillCfg).allowed);

  RateLimitConfig tightCfg = fillCfg;
  tightCfg.maxKeys = 1;
  EXPECT_TRUE(store.consume("ip-d", t0 + std::chrono::seconds{1}, tightCfg).allowed);
}

TEST(RateLimitRedisContractTest, BuildsKeyWithHashTagByDefault) {
  RedisSlidingWindowConfig config;
  config.namespacePrefix = "aeronet:rl";
  config.useHashTag = true;
  RedisSlidingWindowRateLimitStore store({}, std::move(config));
  const auto key = store.buildRedisKey("client-1");
  EXPECT_EQ(key, "aeronet:rl:{client-1}");
}

TEST(RateLimitRedisContractTest, BuildsKeyWithoutHashTagWhenDisabled) {
  RedisSlidingWindowConfig config;
  config.namespacePrefix = "rl";
  config.useHashTag = false;
  RedisSlidingWindowRateLimitStore store({}, std::move(config));

  const auto key = store.buildRedisKey("client-1");

  EXPECT_EQ(key, "rl:client-1");
}

TEST(RateLimitRedisContractTest, BuildsConsumeEvalRequestShape) {
  RedisSlidingWindowConfig redisConfig;
  redisConfig.preferEvalSha = true;
  RedisSlidingWindowRateLimitStore store({}, std::move(redisConfig));
  RateLimitConfig cfg;
  cfg.requestsPerSecond = 5;
  cfg.burst = 10;

  const auto req = store.buildConsumeRequest(
      "client-42", std::chrono::steady_clock::time_point{} + std::chrono::seconds{20000000}, cfg);

  EXPECT_EQ(req.key, "aeronet:rl:{client-42}");
  EXPECT_EQ(req.script, RedisSlidingWindowRateLimitStore::luaSlidingWindowScript());
  EXPECT_EQ(req.scriptSha, RedisSlidingWindowRateLimitStore::luaSlidingWindowScriptSha1());
  EXPECT_TRUE(req.preferEvalSha);
  EXPECT_EQ(req.args[0], "20000000000");
  EXPECT_EQ(req.args[1], "1000");  // window seconds
  EXPECT_EQ(req.args[2], "5");     // tokens to add (1000ms * 5 rps / 1000ms)
}

TEST(RateLimitRedisContractTest, BuildsConsumeEvalRequestWithCustomWindow) {
  RedisSlidingWindowConfig redisConfig;
  redisConfig.namespacePrefix = "rl";
  redisConfig.windowSeconds = 60;
  redisConfig.useHashTag = false;
  RedisSlidingWindowRateLimitStore store({}, std::move(redisConfig));
  RateLimitConfig cfg;
  cfg.requestsPerSecond = 7;

  const auto req = store.buildConsumeRequest(
      "client-42", std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{2000}, cfg);

  EXPECT_EQ(req.key, "rl:client-42");
  EXPECT_FALSE(req.preferEvalSha);
  EXPECT_EQ(req.args[0], "2000");
  EXPECT_EQ(req.args[1], "60000");
  EXPECT_EQ(req.args[2], "420");
}

TEST(RateLimitRedisContractTest, ParsesBackendErrorUsingFailPolicy) {
  RateLimitConfig cfg;
  RedisEvalResponse resp;
  resp.allowed = -1;

  cfg.failOpen = true;
  auto decision = RedisSlidingWindowRateLimitStore::parseConsumeResponse(resp, cfg);
  ASSERT_TRUE(decision.has_value());
  EXPECT_TRUE(decision->allowed);

  cfg.failOpen = false;
  decision = RedisSlidingWindowRateLimitStore::parseConsumeResponse(resp, cfg);
  ASSERT_TRUE(decision.has_value());
  EXPECT_FALSE(decision->allowed);
  EXPECT_EQ(decision->retryAfterSeconds, 1U);
}

TEST(RateLimitRedisContractTest, ParsesAllowedResponse) {
  RateLimitConfig cfg;
  RedisEvalResponse resp;
  resp.allowed = 1;

  const auto decision = RedisSlidingWindowRateLimitStore::parseConsumeResponse(resp, cfg);

  ASSERT_TRUE(decision.has_value());
  EXPECT_TRUE(decision->allowed);
  EXPECT_EQ(decision->retryAfterSeconds, 0U);
}

TEST(RateLimitRedisContractTest, ParsesDeniedResponse) {
  RateLimitConfig cfg;
  RedisEvalResponse resp;
  resp.allowed = 0;
  resp.retryAfterSeconds = 3;

  const auto decision = RedisSlidingWindowRateLimitStore::parseConsumeResponse(resp, cfg);
  ASSERT_TRUE(decision.has_value());
  EXPECT_FALSE(decision->allowed);
  EXPECT_EQ(decision->retryAfterSeconds, 3U);
}

TEST(RateLimitRedisContractTest, ParsesDeniedResponseClampsRetryAfter) {
  RateLimitConfig cfg;
  RedisEvalResponse resp;
  resp.allowed = 0;

  resp.retryAfterSeconds = 0;
  auto decision = RedisSlidingWindowRateLimitStore::parseConsumeResponse(resp, cfg);
  ASSERT_TRUE(decision.has_value());
  EXPECT_FALSE(decision->allowed);
  EXPECT_EQ(decision->retryAfterSeconds, 1U);

  resp.retryAfterSeconds = static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1;
  decision = RedisSlidingWindowRateLimitStore::parseConsumeResponse(resp, cfg);
  ASSERT_TRUE(decision.has_value());
  EXPECT_FALSE(decision->allowed);
  EXPECT_EQ(decision->retryAfterSeconds, std::numeric_limits<uint32_t>::max());
}

TEST(RateLimitRedisContractTest, MalformedAllowedValueReturnsNoDecision) {
  RateLimitConfig cfg;
  RedisEvalResponse resp;
  resp.allowed = 2;

  EXPECT_FALSE(RedisSlidingWindowRateLimitStore::parseConsumeResponse(resp, cfg).has_value());
}

TEST(RateLimitRedisStoreTest, EmptyKeyBypassesCallback) {
  bool called = false;
  RedisSlidingWindowRateLimitStore store([&called](const RedisEvalRequest&) -> std::optional<RedisEvalResponse> {
    called = true;
    return RedisEvalResponse{};
  });
  RateLimitConfig cfg;

  const auto decision = store.consume({}, std::chrono::steady_clock::time_point{}, cfg);

  EXPECT_TRUE(decision.allowed);
  EXPECT_FALSE(called);
}

TEST(RateLimitRedisStoreTest, MissingCallbackUsesFailPolicy) {
  RedisSlidingWindowRateLimitStore store;
  RateLimitConfig cfg;
  const auto now = std::chrono::steady_clock::time_point{};

  cfg.failOpen = true;
  EXPECT_TRUE(store.consume("client", now, cfg).allowed);

  cfg.failOpen = false;
  const auto denied = store.consume("client", now, cfg);
  EXPECT_FALSE(denied.allowed);
  EXPECT_EQ(denied.retryAfterSeconds, 1U);
}

TEST(RateLimitRedisStoreTest, EmptyCallbackResultUsesFailPolicy) {
  RedisSlidingWindowRateLimitStore store(
      [](const RedisEvalRequest&) -> std::optional<RedisEvalResponse> { return std::nullopt; });
  RateLimitConfig cfg;
  const auto now = std::chrono::steady_clock::time_point{};

  cfg.failOpen = true;
  EXPECT_TRUE(store.consume("client", now, cfg).allowed);

  cfg.failOpen = false;
  const auto denied = store.consume("client", now, cfg);
  EXPECT_FALSE(denied.allowed);
  EXPECT_EQ(denied.retryAfterSeconds, 1U);
}

TEST(RateLimitRedisStoreTest, CallbackAllowedResponseAllowsRequest) {
  bool inspected = false;
  RedisSlidingWindowRateLimitStore store([&inspected](const RedisEvalRequest& req) -> std::optional<RedisEvalResponse> {
    inspected = true;
    EXPECT_EQ(req.key, "aeronet:rl:{client}");
    RedisEvalResponse resp;
    resp.allowed = 1;
    return resp;
  });
  RateLimitConfig cfg;

  const auto decision = store.consume("client", std::chrono::steady_clock::time_point{}, cfg);

  EXPECT_TRUE(decision.allowed);
  EXPECT_TRUE(inspected);
}

TEST(RateLimitRedisStoreTest, CallbackDeniedResponseRejectsRequest) {
  RedisSlidingWindowRateLimitStore store([](const RedisEvalRequest&) -> std::optional<RedisEvalResponse> {
    RedisEvalResponse resp;
    resp.allowed = 0;
    resp.retryAfterSeconds = 4;
    return resp;
  });
  RateLimitConfig cfg;

  const auto decision = store.consume("client", std::chrono::steady_clock::time_point{}, cfg);

  EXPECT_FALSE(decision.allowed);
  EXPECT_EQ(decision.retryAfterSeconds, 4U);
}

TEST(RateLimitRedisStoreTest, MalformedCallbackResponseUsesFailPolicy) {
  RedisSlidingWindowRateLimitStore store([](const RedisEvalRequest&) -> std::optional<RedisEvalResponse> {
    RedisEvalResponse resp;
    resp.allowed = 2;
    return resp;
  });
  RateLimitConfig cfg;
  const auto now = std::chrono::steady_clock::time_point{};

  cfg.failOpen = true;
  EXPECT_TRUE(store.consume("client", now, cfg).allowed);

  cfg.failOpen = false;
  const auto denied = store.consume("client", now, cfg);
  EXPECT_FALSE(denied.allowed);
  EXPECT_EQ(denied.retryAfterSeconds, 1U);
}

}  // namespace
}  // namespace aeronet
