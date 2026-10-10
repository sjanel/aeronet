#include "../src/tunnel-resolver.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

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

}  // namespace

TEST(TunnelResolverTest, PendingTunnelKeepsANullTerminatedHost) {
  const PendingTunnel pending("example.com", 443, NativeHandle{3}, 5U, 42U);
  EXPECT_EQ(pending.host(), "example.com");
  EXPECT_EQ(pending.hostBuffer.data()[pending.host().size()], '\0');
  EXPECT_EQ(pending.port, 443);
  EXPECT_EQ(pending.clientFd, NativeHandle{3});
  EXPECT_EQ(pending.streamId, 5U);
  EXPECT_EQ(pending.consumedBytes, 42U);
  EXPECT_FALSE(pending.cancelled);
}

TEST(TunnelResolverTest, ResolvesHostNamesAndNotifies) {
  std::atomic<int> notifications{0};
  TunnelResolver resolver([&notifications] { notifications.fetch_add(1, std::memory_order_relaxed); });
  EXPECT_FALSE(resolver.hasResolved());

  resolver.resolve(std::make_shared<PendingTunnel>("localhost", 80, NativeHandle{1}, 0U, 0U));
  resolver.resolve(std::make_shared<PendingTunnel>("no-such-host.example.invalid", 80, NativeHandle{2}, 0U, 0U));

  const auto resolved = WaitForResolved(resolver, 2);
  ASSERT_EQ(resolved.size(), 2U);
  EXPECT_GE(notifications.load(), 1);
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
  TunnelResolver resolver([] {});
  static constexpr uint32_t kNbResolutions = (3U * TunnelResolver::kMaxThreads) + 1U;
  test::GetAddrInfoDelayGuard getAddrInfoDelayGuard(20);
  for (uint32_t idx = 0; idx < kNbResolutions; ++idx) {
    resolver.resolve(std::make_shared<PendingTunnel>("localhost", 80, static_cast<NativeHandle>(idx), 0U, 0U));
  }
  const auto resolved = WaitForResolved(resolver, kNbResolutions);
  EXPECT_EQ(resolved.size(), kNbResolutions);
}

TEST(TunnelResolverTest, DestructionDoesNotWaitForResolutionsInProgress) {
  std::atomic<int> notifications{0};
  test::GetAddrInfoDelayGuard getAddrInfoDelayGuard(1000);
  const auto start = std::chrono::steady_clock::now();
  {
    TunnelResolver resolver([&notifications] { notifications.fetch_add(1, std::memory_order_relaxed); });
    resolver.resolve(std::make_shared<PendingTunnel>("localhost", 80, NativeHandle{1}, 0U, 0U));
    // Queued behind the first one if a single thread was started, or resolving too: dropped either way.
    resolver.resolve(std::make_shared<PendingTunnel>("localhost", 80, NativeHandle{2}, 0U, 0U));
    std::this_thread::sleep_for(50ms);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 700ms);

  // The resolutions complete after the resolver was destroyed: nobody is notified.
  std::this_thread::sleep_for(1200ms);
  EXPECT_EQ(notifications.load(), 0);
}
#endif

}  // namespace aeronet::internal
