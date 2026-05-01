#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "aeronet/connection-state.hpp"
#include "aeronet/internal/keep-alive-deadline-queue.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

namespace {

constexpr std::int64_t kDefaultConnections = 10'000;
constexpr auto kKeepAliveTimeout = std::chrono::seconds{5};

vector<ConnectionState> MakeConnectionStates(std::int64_t nbConnections,
                                             std::chrono::steady_clock::time_point lastActivity) {
  vector<ConnectionState> states;
  states.resize(static_cast<decltype(states)::size_type>(nbConnections));
  for (ConnectionState& connectionState : states) {
    connectionState.lastActivity = lastActivity;
  }
  return states;
}

void BM_LinearKeepAliveSweepNoExpired(benchmark::State& state) {
  const auto nbConnections = state.range(0);
  const auto base = std::chrono::steady_clock::now();
  auto states = MakeConnectionStates(nbConnections, base);
  const auto now = base + std::chrono::seconds{1};

  for ([[maybe_unused]] auto iteration : state) {
    std::size_t expiredCount = 0;
    for (const ConnectionState& connectionState : states) {
      if (now > connectionState.lastActivity + kKeepAliveTimeout) {
        ++expiredCount;
      }
    }
    benchmark::DoNotOptimize(expiredCount);
  }
}

void BM_DeadlineQueueNoExpired(benchmark::State& state) {
  const auto nbConnections = state.range(0);
  const auto base = std::chrono::steady_clock::now();
  auto states = MakeConnectionStates(nbConnections, base);
  KeepAliveDeadlineQueue queue;
  for (std::int64_t connectionIndex = 0; connectionIndex < nbConnections; ++connectionIndex) {
    queue.upsert(states[static_cast<decltype(states)::size_type>(connectionIndex)],
                 NativeHandle{static_cast<int>(connectionIndex + 1)}, base + kKeepAliveTimeout);
  }
  const auto now = base + std::chrono::seconds{1};

  for ([[maybe_unused]] auto iteration : state) {
    bool hasExpired = !queue.empty() && queue.top().expiresAt <= now;
    benchmark::DoNotOptimize(hasExpired);
  }
}

void BM_DeadlineQueuePopExpired(benchmark::State& state) {
  const auto nbConnections = state.range(0);
  const auto nbExpired = std::min(state.range(1), nbConnections);

  for ([[maybe_unused]] auto iteration : state) {
    state.PauseTiming();
    const auto base = std::chrono::steady_clock::now();
    auto states = MakeConnectionStates(nbConnections, base);
    KeepAliveDeadlineQueue queue;
    for (std::int64_t connectionIndex = 0; connectionIndex < nbConnections; ++connectionIndex) {
      const auto expiry = connectionIndex < nbExpired ? base : base + kKeepAliveTimeout;
      queue.upsert(states[static_cast<decltype(states)::size_type>(connectionIndex)],
                   NativeHandle{static_cast<int>(connectionIndex + 1)}, expiry);
    }
    const auto now = base;
    state.ResumeTiming();

    std::size_t expiredCount = 0;
    while (!queue.empty() && queue.top().expiresAt <= now) {
      benchmark::DoNotOptimize(queue.pop());
      ++expiredCount;
    }
    benchmark::DoNotOptimize(expiredCount);
  }
}

BENCHMARK(BM_LinearKeepAliveSweepNoExpired)->Arg(kDefaultConnections)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_DeadlineQueueNoExpired)->Arg(kDefaultConnections)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_DeadlineQueuePopExpired)->Args({kDefaultConnections, 100})->Unit(benchmark::kNanosecond);

}  // namespace

}  // namespace aeronet::internal

BENCHMARK_MAIN();
