#include "aeronet/rate-limit.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"

namespace aeronet {

void RateLimitConfig::validate() const {
  if (requestsPerSecond == 0) {
    throw std::invalid_argument("RateLimitConfig.requestsPerSecond must be > 0");
  }
  if (burst < requestsPerSecond) {
    throw std::invalid_argument("RateLimitConfig.burst must be >= requestsPerSecond");
  }
  if (maxKeys == 0) {
    throw std::invalid_argument("RateLimitConfig.maxKeys must be > 0");
  }
  if (idleTtl <= std::chrono::seconds::zero()) {
    throw std::invalid_argument("RateLimitConfig.idleTtl must be > 0");
  }
  if (nbShards == 0) {
    throw std::invalid_argument("RateLimitConfig.nbShards cannot be 0");
  }
}

InMemoryTokenBucketRateLimitStore::InMemoryTokenBucketRateLimitStore(uint8_t nbShards)
    : _shards(std::make_unique<Shard[]>(nbShards)), _nbShards(nbShards) {}

void InMemoryTokenBucketRateLimitStore::evictToLimit(const RateLimitConfig& config,
                                                     std::chrono::steady_clock::time_point now,
                                                     std::size_t protectedShardIndex, std::string_view protectedKey) {
  std::scoped_lock evictionLock(_evictionLock);

  for (std::size_t shardIndex = 0; shardIndex < _nbShards && _size.load(std::memory_order_relaxed) > config.maxKeys;
       ++shardIndex) {
    Shard& shard = _shards[shardIndex];
    std::scoped_lock lock(shard.lock);
    for (auto it = shard.buckets.begin();
         it != shard.buckets.end() && _size.load(std::memory_order_relaxed) > config.maxKeys;) {
      const bool protectedEntry = shardIndex == protectedShardIndex && it->first == protectedKey;
      if (!protectedEntry && now - it->second.lastSeen >= config.idleTtl) {
        it = shard.buckets.erase(it);
        _size.fetch_sub(1U, std::memory_order_relaxed);
      } else {
        ++it;
      }
    }
  }

  for (std::size_t shardIndex = 0; shardIndex < _nbShards && _size.load(std::memory_order_relaxed) > config.maxKeys;
       ++shardIndex) {
    Shard& shard = _shards[shardIndex];
    std::scoped_lock lock(shard.lock);
    for (auto it = shard.buckets.begin();
         it != shard.buckets.end() && _size.load(std::memory_order_relaxed) > config.maxKeys;) {
      const bool protectedEntry = shardIndex == protectedShardIndex && it->first == protectedKey;
      if (!protectedEntry) {
        it = shard.buckets.erase(it);
        _size.fetch_sub(1U, std::memory_order_relaxed);
      } else {
        ++it;
      }
    }
  }
}

RateLimitDecision InMemoryTokenBucketRateLimitStore::consume(std::string_view key,
                                                             std::chrono::steady_clock::time_point now,
                                                             const RateLimitConfig& config) {
  if (key.empty()) {
    return RateLimitDecision::Allow();
  }
  const std::size_t shardIndex = CityHash{}(key) % _nbShards;
  Shard& shard = _shards[shardIndex];
  RateLimitDecision decision;
  bool inserted{};

  {
    std::scoped_lock lock(shard.lock);

    if (shard.buckets.empty()) {
      const std::size_t expectedShardKeys = (static_cast<std::size_t>(config.maxKeys) + _nbShards - 1U) / _nbShards;
      shard.buckets.reserve(expectedShardKeys);
    }

    auto [it, wasInserted] = shard.buckets.try_emplace(key, static_cast<double>(config.burst), now);
    inserted = wasInserted;
    if (inserted) {
      _size.fetch_add(1U, std::memory_order_relaxed);
    }

    Bucket& bucket = it->second;
    if (!inserted) {
      bucket.lastSeen = now;

      const auto elapsed = now - bucket.lastRefill;
      if (elapsed > std::chrono::steady_clock::duration::zero()) {
        const double seconds = std::chrono::duration<double>(elapsed).count();
        bucket.tokens =
            std::min(static_cast<double>(config.burst), bucket.tokens + (seconds * config.requestsPerSecond));
        bucket.lastRefill = now;
      }
    }

    if (bucket.tokens >= 1.0) [[likely]] {
      bucket.tokens -= 1.0;
      decision = RateLimitDecision::Allow();
    } else {
      const double missing = 1.0 - bucket.tokens;
      const double waitSecondsRaw = missing / static_cast<double>(config.requestsPerSecond);
      const auto waitSeconds = static_cast<uint32_t>(
          std::clamp(std::ceil(waitSecondsRaw), 1.0, static_cast<double>(std::numeric_limits<uint32_t>::max())));
      decision = RateLimitDecision::Reject(waitSeconds);
    }
  }

  if (inserted && _size.load(std::memory_order_relaxed) > config.maxKeys) [[unlikely]] {
    // bytell_hash_map::erase may relocate a different collision-chain element. Do not access bucket or it after this.
    evictToLimit(config, now, shardIndex, key);
  }

  return decision;
}

namespace {

constexpr std::string_view kRedisSlidingWindowScript = R"LUA(
local key = KEYS[1]
local now_ms = tonumber(ARGV[1])
local window_ms = tonumber(ARGV[2])
local limit = tonumber(ARGV[3])

local from_ms = now_ms - window_ms
redis.call('ZREMRANGEBYSCORE', key, '-inf', from_ms)

local count = redis.call('ZCARD', key)
if count >= limit then
  local oldest = redis.call('ZRANGE', key, 0, 0, 'WITHSCORES')
  local retry = 1
  if oldest[2] ~= nil then
    local wait_ms = window_ms - (now_ms - tonumber(oldest[2]))
    if wait_ms > 0 then
      retry = math.ceil(wait_ms / 1000)
    end
  end
  return {0, retry}
end

local member = tostring(now_ms) .. '-' .. tostring(math.random(1000000, 9999999))
redis.call('ZADD', key, now_ms, member)
redis.call('PEXPIRE', key, window_ms)
return {1, 0}
)LUA";

// Placeholder for adapter wiring. Integrators may pre-load script and set this SHA in their callback layer.
constexpr std::string_view kRedisSlidingWindowScriptSha1;

}  // namespace

std::string_view RedisSlidingWindowRateLimitStore::luaSlidingWindowScript() noexcept {
  return kRedisSlidingWindowScript;
}

std::string_view RedisSlidingWindowRateLimitStore::luaSlidingWindowScriptSha1() noexcept {
  return kRedisSlidingWindowScriptSha1;
}

std::string_view RedisSlidingWindowRateLimitStore::buildRedisKey(std::string_view key) {
  const std::size_t neededSize =
      _redisConfig.namespacePrefix.size() + 1U + key.size() + (_redisConfig.useHashTag ? 2U : 0);
  char* pRedisKey = _charStorage.allocateAndDefaultConstruct(neededSize);

  pRedisKey = Append(_redisConfig.namespacePrefix, pRedisKey);

  *pRedisKey++ = ':';

  if (_redisConfig.useHashTag) {
    *pRedisKey++ = '{';
  }
  pRedisKey = Append(key, pRedisKey);
  if (_redisConfig.useHashTag) {
    *pRedisKey++ = '}';
  }

  return {pRedisKey - neededSize, neededSize};
}

RedisEvalRequest RedisSlidingWindowRateLimitStore::buildConsumeRequest(std::string_view key,
                                                                       std::chrono::steady_clock::time_point now,
                                                                       const RateLimitConfig& config) {
  _charStorage.clear();

  RedisEvalRequest req{
      .script = kRedisSlidingWindowScript,
      .scriptSha = kRedisSlidingWindowScriptSha1,
      .key = buildRedisKey(key),
      .args = {},
      .preferEvalSha = _redisConfig.preferEvalSha,
  };

  const auto nowMs =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
  const auto windowMs = static_cast<uint64_t>(_redisConfig.windowSeconds) * 1000;
  const auto maxInWindow = static_cast<uint64_t>(config.requestsPerSecond) * _redisConfig.windowSeconds;

  const auto nowMsNbDigits = ndigits(nowMs);
  const auto windowMsNbDigits = ndigits(windowMs);
  const auto maxInWindowNbDigits = ndigits(maxInWindow);

  const uint32_t neededSize = static_cast<uint32_t>(nowMsNbDigits + windowMsNbDigits + maxInWindowNbDigits);
  char* pArgs = _charStorage.allocateAndDefaultConstruct(neededSize);

  req.args[0] = std::string_view(pArgs, nowMsNbDigits);
  pArgs = WriteInt(pArgs, nowMs, nowMsNbDigits);

  req.args[1] = std::string_view(pArgs, windowMsNbDigits);
  pArgs = WriteInt(pArgs, windowMs, windowMsNbDigits);

  req.args[2] = std::string_view(pArgs, maxInWindowNbDigits);
  pArgs = WriteInt(pArgs, maxInWindow, maxInWindowNbDigits);

  return req;
}

RateLimitDecision RedisSlidingWindowRateLimitStore::parseConsumeResponse(const RedisEvalResponse& response,
                                                                         const RateLimitConfig& config) {
  if (response.allowed < 0) {
    return config.failOpen ? RateLimitDecision::Allow() : RateLimitDecision::Reject(1);
  }

  if (response.allowed == 1) {
    return RateLimitDecision::Allow();
  }

  if (response.allowed != 0) {
    return RateLimitDecision::Invalid();
  }

  const auto retryAfter =
      static_cast<uint32_t>(std::clamp<int64_t>(response.retryAfterSeconds, 1, std::numeric_limits<uint32_t>::max()));
  return RateLimitDecision::Reject(retryAfter);
}

RateLimitDecision RedisSlidingWindowRateLimitStore::consume(std::string_view key,
                                                            std::chrono::steady_clock::time_point now,
                                                            const RateLimitConfig& config) {
  if (key.empty()) {
    return RateLimitDecision::Allow();
  }

  if (!_callback) {
    return config.failOpen ? RateLimitDecision::Allow() : RateLimitDecision::Reject(1);
  }

  const RedisEvalRequest request = buildConsumeRequest(key, now, config);
  const auto decision = parseConsumeResponse(_callback(request), config);
  if (decision.invalid()) {
    return config.failOpen ? RateLimitDecision::Allow() : RateLimitDecision::Reject(1);
  }
  return decision;
}

}  // namespace aeronet
