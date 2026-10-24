#include "../src/tunnel-resolver.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/event-fd.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_POSIX
// Its getaddrinfo override delays the resolutions.
#define AERONET_WANT_SOCKET_OVERRIDES
#include "aeronet/sys-test-support.hpp"
#endif

namespace aeronet::internal {

namespace {

using namespace std::chrono_literals;

// The resolver ignores the deadline, which the TunnelManager checks.
constexpr std::chrono::steady_clock::time_point kNoDeadline{};

// Collects the resolved tunnels until 'expected' were received, or the timeout elapsed.
vector<std::shared_ptr<PendingTunnel>> WaitForResolved(TunnelResolver& resolver, std::size_t expected,
                                                       std::chrono::milliseconds timeout = 10s) {
  vector<std::shared_ptr<PendingTunnel>> all;
  vector<std::shared_ptr<PendingTunnel>> batch;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (all.size() < expected && std::chrono::steady_clock::now() < deadline) {
    if (resolver.hasResolved()) {
      resolver.takeResolved(batch);
      for (auto& pending : batch) {
        all.push_back(std::move(pending));
      }
    } else {
      std::this_thread::sleep_for(1ms);
    }
  }
  return all;
}

// Tells whether 'wakeupFd' was signaled, without waiting.
bool IsSignaled(const EventFd& wakeupFd) {
  EventLoop loop(PollTimeoutPolicy{.baseTimeout = 1ms});
  loop.addOrThrow(EventLoop::EventFd{wakeupFd.fd(), EventIn});
  return !loop.poll(false).empty();
}

}  // namespace

TEST(TunnelResolverTest, PendingTunnelKeepsANullTerminatedHost) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  const PendingTunnel pending("example.com", 443, NativeHandle{3}, 5U, 42U, deadline);
  EXPECT_EQ(pending.host(), "example.com");
  EXPECT_EQ(pending.hostBuffer.data()[pending.host().size()], '\0');
  EXPECT_EQ(pending.port, 443);
  EXPECT_EQ(pending.clientFd, NativeHandle{3});
  EXPECT_EQ(pending.streamId, 5U);
  EXPECT_EQ(pending.consumedBytes, 42U);
  EXPECT_EQ(pending.deadline, deadline);
  EXPECT_FALSE(pending.cancelled);
}

TEST(TunnelResolverTest, ResolvesHostNamesAndNotifies) {
  EventFd wakeupFd;
  TunnelResolver resolver(wakeupFd);
  EXPECT_FALSE(resolver.hasResolved());
  EXPECT_FALSE(IsSignaled(wakeupFd));

  resolver.resolve(std::make_shared<PendingTunnel>("localhost", 80, NativeHandle{1}, 0U, 0U, kNoDeadline));
  resolver.resolve(
      std::make_shared<PendingTunnel>("no-such-host.example.invalid", 80, NativeHandle{2}, 0U, 0U, kNoDeadline));

  const auto resolved = WaitForResolved(resolver, 2);
  ASSERT_EQ(resolved.size(), 2U);
  EXPECT_TRUE(IsSignaled(wakeupFd));
  EXPECT_FALSE(resolver.hasResolved());
  for (const auto& pending : resolved) {
    if (pending->clientFd == NativeHandle{1}) {
      EXPECT_NE(pending->addresses, nullptr);
    } else {
      // A failed resolution is reported too, without addresses.
      EXPECT_EQ(pending->addresses, nullptr);
    }
  }
}

#ifdef AERONET_POSIX
TEST(TunnelResolverTest, MoreResolutionsThanThreads) {
  EventFd wakeupFd;
  TunnelResolver resolver(wakeupFd);
  static constexpr uint32_t kNbResolutions = (3U * TunnelResolver::kMaxThreads) + 1U;
  test::GetAddrInfoDelayGuard getAddrInfoDelayGuard(20);
  for (uint32_t idx = 0; idx < kNbResolutions; ++idx) {
    resolver.resolve(
        std::make_shared<PendingTunnel>("localhost", 80, static_cast<NativeHandle>(idx), 0U, 0U, kNoDeadline));
  }
  const auto resolved = WaitForResolved(resolver, kNbResolutions);
  EXPECT_EQ(resolved.size(), kNbResolutions);
}

TEST(TunnelResolverTest, CancelDropsQueuedResolutions) {
  EventFd wakeupFd;
  TunnelResolver resolver(wakeupFd);
  test::GetAddrInfoDelayGuard getAddrInfoDelayGuard(200);
  vector<std::shared_ptr<PendingTunnel>> pendings;
  for (uint32_t idx = 0; idx < TunnelResolver::kMaxThreads + 2U; ++idx) {
    pendings.push_back(
        std::make_shared<PendingTunnel>("localhost", 80, static_cast<NativeHandle>(idx), 0U, 0U, kNoDeadline));
    resolver.resolve(pendings.back());
  }
  std::this_thread::sleep_for(50ms);  // the first ones are being resolved: the last two wait in the queue
  resolver.cancel(pendings[TunnelResolver::kMaxThreads]);
  resolver.cancel(pendings[TunnelResolver::kMaxThreads + 1U]);
  // A resolution in progress is not interrupted.
  resolver.cancel(pendings.front());

  const auto resolved = WaitForResolved(resolver, pendings.size(), 1500ms);
  EXPECT_EQ(resolved.size(), static_cast<std::size_t>(TunnelResolver::kMaxThreads));
  for (const auto& pending : resolved) {
    EXPECT_LT(pending->clientFd, static_cast<NativeHandle>(TunnelResolver::kMaxThreads));
  }
}

TEST(TunnelResolverTest, DestructionDoesNotWaitForResolutionsInProgress) {
  EventFd wakeupFd;
  test::GetAddrInfoDelayGuard getAddrInfoDelayGuard(1000);
  const auto start = std::chrono::steady_clock::now();
  {
    TunnelResolver resolver(wakeupFd);
    resolver.resolve(std::make_shared<PendingTunnel>("localhost", 80, NativeHandle{1}, 0U, 0U, kNoDeadline));
    // Queued behind the first one if a single thread was started, or resolving too: dropped either way.
    resolver.resolve(std::make_shared<PendingTunnel>("localhost", 80, NativeHandle{2}, 0U, 0U, kNoDeadline));
    std::this_thread::sleep_for(50ms);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 700ms);

  // The resolutions complete after the resolver was destroyed: the event loop is not woken up.
  std::this_thread::sleep_for(1200ms);
  EXPECT_FALSE(IsSignaled(wakeupFd));
}
#endif

}  // namespace aeronet::internal
