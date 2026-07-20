#include "aeronet/rate-limit.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>

#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/nchars.hpp"

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
}

void InMemoryTokenBucketRateLimitStore::evictOne(const RateLimitConfig& config,
                                                 std::chrono::steady_clock::time_point now) {
  if (_buckets.size() < config.maxKeys) {
    return;
  }

  for (auto it = _buckets.begin(); it != _buckets.end();) {
    if (now - it->second.lastSeen >= config.idleTtl) {
      it = _buckets.erase(it);
      if (_buckets.size() < config.maxKeys) {
        return;
      }
    } else {
      ++it;
    }
  }

  if (!_buckets.empty()) {
    _buckets.erase(_buckets.begin());
  }
}

RateLimitDecision InMemoryTokenBucketRateLimitStore::consume(std::string_view key,
                                                             std::chrono::steady_clock::time_point now,
                                                             const RateLimitConfig& config) {
  if (key.empty()) {
    return RateLimitDecision::Allow();
  }

  std::scoped_lock lock(_lock);

  auto it = _buckets.find(key);
  if (it == _buckets.end()) {
    evictOne(config, now);

    it = _buckets.try_emplace(key).first;

    Bucket& fresh = it->second;
    fresh.tokens = static_cast<double>(config.burst);
    fresh.lastRefill = now;
  }

  Bucket& bucket = it->second;
  bucket.lastSeen = now;

  const auto elapsed = now - bucket.lastRefill;
  if (elapsed > std::chrono::steady_clock::duration::zero()) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    bucket.tokens = std::min(static_cast<double>(config.burst), bucket.tokens + (seconds * config.requestsPerSecond));
    bucket.lastRefill = now;
  }

  if (bucket.tokens >= 1.0) {
    bucket.tokens -= 1.0;
    return RateLimitDecision::Allow();
  }

  const double missing = 1.0 - bucket.tokens;
  const double waitSecondsRaw = missing / static_cast<double>(config.requestsPerSecond);
  const auto waitSeconds = static_cast<uint32_t>(
      std::clamp(std::ceil(waitSecondsRaw), 1.0, static_cast<double>(std::numeric_limits<uint32_t>::max())));
  return RateLimitDecision::Reject(waitSeconds);
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

  const auto nowMsNbChars = nchars(nowMs);
  const auto windowMsNbChars = nchars(windowMs);
  const auto maxInWindowNbChars = nchars(maxInWindow);

  const uint32_t neededSize = static_cast<uint32_t>(nowMsNbChars + windowMsNbChars + maxInWindowNbChars);
  char* pArgs = _charStorage.allocateAndDefaultConstruct(neededSize);

  req.args[0] = std::string_view(pArgs, nowMsNbChars);
  pArgs = std::to_chars(pArgs, pArgs + nowMsNbChars, nowMs).ptr;

  req.args[1] = std::string_view(pArgs, windowMsNbChars);
  pArgs = std::to_chars(pArgs, pArgs + windowMsNbChars, windowMs).ptr;

  req.args[2] = std::string_view(pArgs, maxInWindowNbChars);
  pArgs = std::to_chars(pArgs, pArgs + maxInWindowNbChars, maxInWindow).ptr;

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
