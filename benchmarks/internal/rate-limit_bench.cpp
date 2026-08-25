#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/rate-limit.hpp"

namespace aeronet {
namespace {

using Clock = std::chrono::steady_clock;

RateLimitConfig MakeConfig(std::uint32_t maxKeys) {
  RateLimitConfig config;
  config.requestsPerSecond = 1;
  config.burst = std::numeric_limits<std::uint32_t>::max();
  config.maxKeys = maxKeys;
  return config;
}

void BM_RateLimitConstruct(benchmark::State& state) {
  const auto shardCount = static_cast<std::size_t>(state.range(0));
  for ([[maybe_unused]] auto iteration : state) {
    InMemoryTokenBucketRateLimitStore store(shardCount);
    benchmark::DoNotOptimize(&store);
  }
}

void BM_RateLimitExistingKey(benchmark::State& state) {
  InMemoryTokenBucketRateLimitStore store(64U);
  const RateLimitConfig config = MakeConfig(1024);
  const auto now = Clock::time_point{};
  benchmark::DoNotOptimize(store.consume("client", now, config));

  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(store.consume("client", now, config));
  }
}

void BM_RateLimitInsertKeys(benchmark::State& state) {
  const auto keyCount = static_cast<std::size_t>(state.range(0));
  std::vector<std::string> keys;
  keys.reserve(keyCount);
  for (std::size_t keyIndex = 0; keyIndex < keyCount; ++keyIndex) {
    keys.emplace_back("client-" + std::to_string(keyIndex));
  }

  const RateLimitConfig config = MakeConfig(static_cast<std::uint32_t>(keyCount));
  const auto now = Clock::time_point{};
  for ([[maybe_unused]] auto iteration : state) {
    state.PauseTiming();
    InMemoryTokenBucketRateLimitStore store(64U);
    state.ResumeTiming();

    for (const std::string& key : keys) {
      benchmark::DoNotOptimize(store.consume(key, now, config));
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(keyCount));
}

void BM_RateLimitInsertAtCapacity(benchmark::State& state) {
  constexpr std::string_view oldKey = "old-client";
  constexpr std::string_view newKey = "new-client";
  const RateLimitConfig config = MakeConfig(1);
  const auto now = Clock::time_point{};

  for ([[maybe_unused]] auto iteration : state) {
    state.PauseTiming();
    InMemoryTokenBucketRateLimitStore store(64U);
    benchmark::DoNotOptimize(store.consume(oldKey, now, config));
    state.ResumeTiming();

    benchmark::DoNotOptimize(store.consume(newKey, now, config));
  }
}

void BM_RateLimitContended(benchmark::State& state) {
  static InMemoryTokenBucketRateLimitStore store(64U);
  static const RateLimitConfig config = MakeConfig(1024);
  static constexpr std::string_view keys[]{"client-0", "client-1", "client-2", "client-3",
                                           "client-4", "client-5", "client-6", "client-7"};
  const auto now = Clock::time_point{};
  const auto key = keys[static_cast<std::size_t>(state.thread_index())];
  benchmark::DoNotOptimize(store.consume(key, now, config));

  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(store.consume(key, now, config));
  }
}

BENCHMARK(BM_RateLimitConstruct)->Arg(16)->Arg(32)->Arg(64);
BENCHMARK(BM_RateLimitExistingKey);
BENCHMARK(BM_RateLimitInsertKeys)->Arg(64)->Arg(1024)->Arg(65536);
BENCHMARK(BM_RateLimitInsertAtCapacity);
BENCHMARK(BM_RateLimitContended)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
