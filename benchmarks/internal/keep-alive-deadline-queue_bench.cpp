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

using Clock = std::chrono::steady_clock;

constexpr auto kKeepAliveTimeout = std::chrono::seconds{5};
constexpr auto kTickStep = std::chrono::milliseconds{1};
constexpr auto kInitialTickOffset = std::chrono::seconds{1};
constexpr auto kNoExpiryTickWindow =
    std::chrono::duration_cast<std::chrono::milliseconds>(kKeepAliveTimeout - kInitialTickOffset).count();

struct DeadlineQueueSimulation {
  vector<ConnectionState> states;
  KeepAliveDeadlineQueue queue;
};

auto MakeConnectionStates(std::int64_t nbConnections, Clock::time_point lastActivity) {
  vector<ConnectionState> states(static_cast<decltype(states)::size_type>(nbConnections));
  for (ConnectionState& connectionState : states) {
    connectionState.lastActivity = lastActivity;
  }
  return states;
}

auto MakeDeadlineQueueSimulation(std::int64_t nbConnections, Clock::time_point lastActivity) {
  DeadlineQueueSimulation simulation{.states = MakeConnectionStates(nbConnections, lastActivity), .queue = {}};
  for (std::int64_t connectionIndex = 0; connectionIndex < nbConnections; ++connectionIndex) {
    ConnectionState& connectionState = simulation.states[static_cast<std::size_t>(connectionIndex)];
    simulation.queue.upsert(connectionState, NativeHandle{static_cast<int>(connectionIndex + 1)},
                            lastActivity + kKeepAliveTimeout);
  }
  return simulation;
}

void RefreshConnections(vector<ConnectionState>& states, Clock::time_point now, std::int64_t nbRefreshed,
                        std::int64_t& nextConnectionIndex) {
  if (states.empty() || nbRefreshed <= 0) {
    return;
  }

  const auto statesSize = static_cast<std::int64_t>(states.size());
  for (std::int64_t refreshedIndex = 0; refreshedIndex < nbRefreshed; ++refreshedIndex) {
    const auto connectionIndex = nextConnectionIndex;
    states[static_cast<std::size_t>(connectionIndex)].lastActivity = now;
    nextConnectionIndex = (nextConnectionIndex + 1) % statesSize;
  }
}

void RefreshConnections(DeadlineQueueSimulation& simulation, Clock::time_point now, std::int64_t nbRefreshed,
                        std::int64_t& nextConnectionIndex) {
  if (simulation.states.empty() || nbRefreshed <= 0) {
    return;
  }

  const auto statesSize = static_cast<std::int64_t>(simulation.states.size());
  for (std::int64_t refreshedIndex = 0; refreshedIndex < nbRefreshed; ++refreshedIndex) {
    const auto connectionIndex = nextConnectionIndex;
    ConnectionState& connectionState = simulation.states[static_cast<std::size_t>(connectionIndex)];
    connectionState.lastActivity = now;
    simulation.queue.upsert(connectionState, NativeHandle{static_cast<int>(connectionIndex + 1)},
                            now + kKeepAliveTimeout);
    nextConnectionIndex = (nextConnectionIndex + 1) % statesSize;
  }
}

void BM_LinearKeepAliveSweepNoExpired(benchmark::State& state) {
  const auto nbConnections = state.range(0);
  const auto base = Clock::now();
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

void BM_DeadlineQueuePeekNoExpired(benchmark::State& state) {
  const auto nbConnections = state.range(0);
  const auto base = Clock::now();
  auto simulation = MakeDeadlineQueueSimulation(nbConnections, base);
  const auto now = base + std::chrono::seconds{1};

  for ([[maybe_unused]] auto iteration : state) {
    bool hasExpired = !simulation.queue.empty() && simulation.queue.top().expiresAt <= now;
    benchmark::DoNotOptimize(hasExpired);
  }
}

void BM_LinearKeepAliveTickNoExpired(benchmark::State& state) {
  const auto nbConnections = state.range(0);
  const auto nbRefreshed = std::min(state.range(1), nbConnections);
  auto base = Clock::now();
  auto states = MakeConnectionStates(nbConnections, base);
  std::int64_t nextConnectionIndex = 0;
  std::int64_t tickIndex = 0;

  for ([[maybe_unused]] auto iteration : state) {
    if (tickIndex == kNoExpiryTickWindow) {
      state.PauseTiming();
      base = Clock::now();
      states = MakeConnectionStates(nbConnections, base);
      nextConnectionIndex = 0;
      tickIndex = 0;
      state.ResumeTiming();
    }

    const auto now = base + kInitialTickOffset + (tickIndex * kTickStep);

    RefreshConnections(states, now, nbRefreshed, nextConnectionIndex);

    std::size_t expiredCount = 0;
    for (const ConnectionState& connectionState : states) {
      if (now > connectionState.lastActivity + kKeepAliveTimeout) {
        ++expiredCount;
      }
    }
    benchmark::DoNotOptimize(expiredCount);
    ++tickIndex;
  }
}

void BM_DeadlineQueueTickNoExpired(benchmark::State& state) {
  const auto nbConnections = state.range(0);
  const auto nbRefreshed = std::min(state.range(1), nbConnections);
  auto base = Clock::now();
  auto simulation = MakeDeadlineQueueSimulation(nbConnections, base);
  std::int64_t nextConnectionIndex = 0;
  std::int64_t tickIndex = 0;

  for ([[maybe_unused]] auto iteration : state) {
    if (tickIndex == kNoExpiryTickWindow) {
      state.PauseTiming();
      base = Clock::now();
      simulation = MakeDeadlineQueueSimulation(nbConnections, base);
      nextConnectionIndex = 0;
      tickIndex = 0;
      state.ResumeTiming();
    }

    const auto now = base + kInitialTickOffset + (tickIndex * kTickStep);

    RefreshConnections(simulation, now, nbRefreshed, nextConnectionIndex);

    bool hasExpired = !simulation.queue.empty() && simulation.queue.top().expiresAt <= now;
    benchmark::DoNotOptimize(hasExpired);
    ++tickIndex;
  }
}

void BM_DeadlineQueuePopExpired(benchmark::State& state) {
  const auto nbConnections = state.range(0);
  const auto nbExpired = std::min(state.range(1), nbConnections);

  for ([[maybe_unused]] auto iteration : state) {
    state.PauseTiming();
    const auto base = Clock::now();
    auto simulation = MakeDeadlineQueueSimulation(nbConnections, base + kKeepAliveTimeout);
    for (std::int64_t connectionIndex = 0; connectionIndex < nbExpired; ++connectionIndex) {
      ConnectionState& connectionState = simulation.states[static_cast<std::size_t>(connectionIndex)];
      connectionState.lastActivity = base - kKeepAliveTimeout;
      simulation.queue.upsert(connectionState, NativeHandle{static_cast<int>(connectionIndex + 1)}, base);
    }
    const auto now = base;
    state.ResumeTiming();

    std::size_t expiredCount = 0;
    while (!simulation.queue.empty() && simulation.queue.top().expiresAt <= now) {
      benchmark::DoNotOptimize(simulation.queue.pop());
      ++expiredCount;
    }
    benchmark::DoNotOptimize(expiredCount);
  }
}

BENCHMARK(BM_LinearKeepAliveSweepNoExpired)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_DeadlineQueuePeekNoExpired)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_LinearKeepAliveTickNoExpired)
    ->Args({1000, 0})
    ->Args({1000, 1})
    ->Args({1000, 10})
    ->Args({1000, 100})
    ->Args({100000, 10})
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_DeadlineQueueTickNoExpired)
    ->Args({1000, 0})
    ->Args({1000, 1})
    ->Args({1000, 10})
    ->Args({1000, 100})
    ->Args({100000, 10})
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_DeadlineQueuePopExpired)
    ->Args({10, 100})
    ->Args({100, 100})
    ->Args({1000, 100})
    ->Args({10000, 100})
    ->Args({100000, 100})
    ->Unit(benchmark::kNanosecond);

}  // namespace

}  // namespace aeronet::internal

BENCHMARK_MAIN();
