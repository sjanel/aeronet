#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <ranges>

#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/vector.hpp"

#define AERONET_WANT_SOCKET_OVERRIDES
#include "aeronet/sys-test-support.hpp"

#ifdef AERONET_LINUX
#include <sys/epoll.h>

namespace aeronet {

namespace {

constexpr uint32_t kBenchCapacity = 64;

EventLoop::PollTimeoutPolicy AdaptivePolicy() {
  return EventLoop::PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{1},
                                      .minTimeout = std::chrono::milliseconds{0},
                                      .maxTimeout = std::chrono::milliseconds{64}};
}

vector<epoll_event> ReadyEvents(uint32_t nbEvents) {
  vector<epoll_event> events;
  events.reserve(nbEvents);
  for (const auto eventIndex : std::views::iota(0U, nbEvents)) {
    events.push_back(test::MakeEvent(static_cast<int>(eventIndex), EventIn));
  }
  return events;
}

void BM_FixedIdlePoll(benchmark::State& state) {
  test::EventLoopHookGuard guard;
  EventLoop loop(EventLoop::PollTimeoutPolicy{.baseTimeout = std::chrono::milliseconds{1},
                                              .minTimeout = std::chrono::milliseconds{0},
                                              .maxTimeout = std::chrono::milliseconds{1}},
                 kBenchCapacity);
  test::SetEpollWaitActions({test::WaitReturn(0, {})});

  for ([[maybe_unused]] auto iteration : state) {
    const auto events = loop.poll();
    benchmark::DoNotOptimize(events.data());
    benchmark::DoNotOptimize(events.size());
  }
}

void BM_AdaptiveIdleBackoffPoll(benchmark::State& state) {
  test::EventLoopHookGuard guard;
  EventLoop loop(AdaptivePolicy(), kBenchCapacity);
  test::SetEpollWaitActions({test::WaitReturn(0, {})});

  for ([[maybe_unused]] auto iteration : state) {
    const auto events = loop.poll();
    benchmark::DoNotOptimize(events.data());
    benchmark::DoNotOptimize(events.size());
    benchmark::DoNotOptimize(loop.currentPollTimeoutMs());
  }
}

void BM_AdaptiveReadyPoll(benchmark::State& state) {
  test::EventLoopHookGuard guard;
  EventLoop loop(AdaptivePolicy(), kBenchCapacity);
  test::SetEpollWaitActions({test::WaitReturn(1, ReadyEvents(1))});

  for ([[maybe_unused]] auto iteration : state) {
    const auto events = loop.poll();
    benchmark::DoNotOptimize(events.data());
    benchmark::DoNotOptimize(events.size());
    benchmark::DoNotOptimize(loop.currentPollTimeoutMs());
  }
}

BENCHMARK(BM_FixedIdlePoll)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_AdaptiveIdleBackoffPoll)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_AdaptiveReadyPoll)->Unit(benchmark::kNanosecond);

}  // namespace

}  // namespace aeronet

BENCHMARK_MAIN();

#endif  // AERONET_LINUX