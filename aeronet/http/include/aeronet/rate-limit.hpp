#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "aeronet/city-hash.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/object-array-pool.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

struct RateLimitConfig {
  void validate() const;

  // Idle key eviction threshold.
  std::chrono::seconds idleTtl{std::chrono::seconds{300}};
  // Sustained token refill rate in requests per second. Must be > 0.
  uint32_t requestsPerSecond{10};
  // Maximum bucket size in tokens. Must be >= requestsPerSecond.
  uint32_t burst{10};
  // Upper bound for in-memory key count.
  uint32_t maxKeys{65536};
  // When store/backing callback errors, fail-open continues request processing.
  bool failOpen{true};
};

struct RateLimitDecision {
  enum class Status : uint8_t { Allowed, Rejected, Invalid };

  [[nodiscard]] static RateLimitDecision Allow() noexcept { return {Status::Allowed, 0}; }

  [[nodiscard]] static RateLimitDecision Reject(uint32_t retryAfterSeconds) noexcept {
    return {Status::Rejected, retryAfterSeconds};
  }

  [[nodiscard]] static RateLimitDecision Invalid() noexcept { return {Status::Invalid, 0}; }

  [[nodiscard]] bool allowed() const noexcept { return status == Status::Allowed; }
  [[nodiscard]] bool rejected() const noexcept { return status == Status::Rejected; }
  [[nodiscard]] bool invalid() const noexcept { return status == Status::Invalid; }

  Status status{Status::Allowed};
  uint32_t retryAfterSeconds{0};
};

class IRateLimitStore {
 public:
  virtual ~IRateLimitStore() = default;

  virtual RateLimitDecision consume(std::string_view key, std::chrono::steady_clock::time_point now,
                                    const RateLimitConfig& config) = 0;
};

class InMemoryTokenBucketRateLimitStore final : public IRateLimitStore {
 public:
  InMemoryTokenBucketRateLimitStore() = default;

  RateLimitDecision consume(std::string_view key, std::chrono::steady_clock::time_point now,
                            const RateLimitConfig& config) override;

 private:
  struct Bucket {
    double tokens{};
    std::chrono::steady_clock::time_point lastRefill;
    std::chrono::steady_clock::time_point lastSeen;
  };

  void evictOne(const RateLimitConfig& config, std::chrono::steady_clock::time_point now);

  flat_hash_map<RawChars32, Bucket, CityHash, std::equal_to<>> _buckets;
  std::mutex _lock;
};

// Optional Redis-backed sliding-window contract.
//
// This does not link any Redis client implementation directly. Instead it exposes a concrete
// eval request/response boundary that can be adapted to hiredis, redis-plus-plus, etc.
//
// Script contract:
//   KEYS[1] = rate-limit key (single key for hash-slot affinity)
//   ARGV[1] = now_millis
//   ARGV[2] = window_millis
//   ARGV[3] = limit (max requests allowed in window)
// Returns:
//   [allowed(0|1), retry_after_seconds]
struct RedisEvalRequest {
  std::string_view script;
  std::string_view scriptSha;
  std::string_view key;
  std::string_view args[3U];
  bool preferEvalSha{};
};

struct RedisEvalResponse {
  // < 0: backend/adapter error, 0: rejected, 1: allowed.
  int64_t allowed{-1};
  int64_t retryAfterSeconds{0};
};

struct RedisSlidingWindowConfig {
  // Namespace prefix for keys (for example "aeronet:rl").
  std::string_view namespacePrefix{"aeronet:rl"};
  // Fixed sliding-window size in seconds.
  uint32_t windowSeconds{1};
  // If true, key uses Redis hash tag format to keep a client's key in one slot.
  bool useHashTag{true};
  // Prefer EVALSHA over EVAL when scriptSha is set.
  bool preferEvalSha{false};
};

class RedisSlidingWindowRateLimitStore final : public IRateLimitStore {
 public:
  using EvalCallback = std::function<RedisEvalResponse(const RedisEvalRequest&)>;

  RedisSlidingWindowRateLimitStore() noexcept = default;

  explicit RedisSlidingWindowRateLimitStore(EvalCallback callback, RedisSlidingWindowConfig config = {})
      : _callback(std::move(callback)), _redisConfig(std::move(config)) {}

  std::string_view buildRedisKey(std::string_view key);

  // Boundary helpers for adapters.
  [[nodiscard]] RedisEvalRequest buildConsumeRequest(std::string_view key, std::chrono::steady_clock::time_point now,
                                                     const RateLimitConfig& config);

  [[nodiscard]] static RateLimitDecision parseConsumeResponse(const RedisEvalResponse& response,
                                                              const RateLimitConfig& config);

  [[nodiscard]] static std::string_view luaSlidingWindowScript() noexcept;
  [[nodiscard]] static std::string_view luaSlidingWindowScriptSha1() noexcept;

  RateLimitDecision consume(std::string_view key, std::chrono::steady_clock::time_point now,
                            const RateLimitConfig& config) override;

 private:
  EvalCallback _callback;
  RedisSlidingWindowConfig _redisConfig;
  ObjectArrayPool<char> _charStorage;
};

using RateLimitStorePtr = std::shared_ptr<IRateLimitStore>;

}  // namespace aeronet
