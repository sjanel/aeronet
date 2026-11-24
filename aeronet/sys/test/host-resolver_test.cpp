#include "aeronet/host-resolver.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/event-fd.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_POSIX
// Its getaddrinfo override delays the resolutions.
#define AERONET_WANT_SOCKET_OVERRIDES
#include "aeronet/sys-test-support.hpp"
#endif

namespace aeronet::internal {

namespace {

using namespace std::chrono_literals;

// A resolution telling its requester apart.
struct TaggedResolution : HostResolution {
  TaggedResolution(std::string_view host, uint32_t requester) : HostResolution(host, 80), tag(requester) {}

  uint32_t tag;
};

std::shared_ptr<TaggedResolution> MakeResolution(std::string_view host, uint32_t tag) {
  return std::make_shared<TaggedResolution>(host, tag);
}

uint32_t TagOf(const std::shared_ptr<HostResolution>& resolution) {
  return static_cast<const TaggedResolution&>(*resolution).tag;
}

// Collects the resolutions done until 'expected' were received, or the timeout elapsed.
vector<std::shared_ptr<HostResolution>> WaitForResolved(HostResolver& resolver, std::size_t expected,
                                                        std::chrono::milliseconds timeout = 10s) {
  vector<std::shared_ptr<HostResolution>> all;
  vector<std::shared_ptr<HostResolution>> batch;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (all.size() < expected && std::chrono::steady_clock::now() < deadline) {
    if (resolver.hasResolved()) {
      resolver.takeResolved(batch);
      for (auto& resolution : batch) {
        all.push_back(std::move(resolution));
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

TEST(HostResolverTest, ResolutionKeepsANullTerminatedHost) {
  const HostResolution resolution("example.com", 443);
  EXPECT_EQ(resolution.host(), "example.com");
  EXPECT_EQ(resolution.hostBuffer.data()[resolution.host().size()], '\0');
  EXPECT_EQ(resolution.port, 443);
  EXPECT_EQ(resolution.addresses, nullptr);
}

TEST(HostResolverTest, ResolvesHostNamesAndNotifies) {
  EventFd wakeupFd;
  HostResolver resolver(wakeupFd);
  EXPECT_FALSE(resolver.hasResolved());
  EXPECT_FALSE(IsSignaled(wakeupFd));

  resolver.resolve(MakeResolution("localhost", 1));
  // On POSIX, the getaddrinfo override fails it without querying the DNS (stalled over 10 s on a macOS CI runner).
  resolver.resolve(MakeResolution("no-such-host.example.invalid", 2));

  const auto resolved = WaitForResolved(resolver, 2);
  ASSERT_EQ(resolved.size(), 2U);
  EXPECT_TRUE(IsSignaled(wakeupFd));
  EXPECT_FALSE(resolver.hasResolved());
  for (const auto& resolution : resolved) {
    if (TagOf(resolution) == 1) {
      EXPECT_NE(resolution->addresses, nullptr);
    } else {
      // A failed resolution is reported too, without addresses.
      EXPECT_EQ(resolution->addresses, nullptr);
    }
  }
}

#ifdef AERONET_POSIX
// The names the getaddrinfo override fails without querying the DNS.
TEST(HostResolverTest, GetAddrInfoOverrideRecognizesInvalidTldHosts) {
  EXPECT_TRUE(test::IsInvalidTldHost("invalid"));
  EXPECT_TRUE(test::IsInvalidTldHost("invalid."));
  EXPECT_TRUE(test::IsInvalidTldHost("no-such-host.example.invalid"));
  EXPECT_TRUE(test::IsInvalidTldHost("No-Such-Host.INVALID."));
  EXPECT_FALSE(test::IsInvalidTldHost(""));
  EXPECT_FALSE(test::IsInvalidTldHost("."));
  EXPECT_FALSE(test::IsInvalidTldHost("localhost"));
  EXPECT_FALSE(test::IsInvalidTldHost("notinvalid"));
  EXPECT_FALSE(test::IsInvalidTldHost("host.notinvalid"));
  EXPECT_FALSE(test::IsInvalidTldHost("host.invalid.example"));
  EXPECT_FALSE(test::IsInvalidTldHost("host.invalid.."));
}

TEST(HostResolverTest, MoreResolutionsThanThreads) {
  EventFd wakeupFd;
  HostResolver resolver(wakeupFd);
  static constexpr uint32_t kNbResolutions = (3U * HostResolver::kMaxThreads) + 1U;
  test::GetAddrInfoDelayGuard getAddrInfoDelayGuard(20);
  for (uint32_t idx = 0; idx < kNbResolutions; ++idx) {
    resolver.resolve(MakeResolution("localhost", idx));
  }
  const auto resolved = WaitForResolved(resolver, kNbResolutions);
  EXPECT_EQ(resolved.size(), kNbResolutions);
}

TEST(HostResolverTest, CancelDropsQueuedResolutions) {
  EventFd wakeupFd;
  HostResolver resolver(wakeupFd);
  test::GetAddrInfoDelayGuard getAddrInfoDelayGuard(200);
  vector<std::shared_ptr<HostResolution>> pendings;
  for (uint32_t idx = 0; idx < HostResolver::kMaxThreads + 2U; ++idx) {
    pendings.push_back(MakeResolution("localhost", idx));
    resolver.resolve(pendings.back());
  }
  std::this_thread::sleep_for(50ms);  // the first ones are being resolved: the last two wait in the queue
  resolver.cancel(*pendings[HostResolver::kMaxThreads]);
  resolver.cancel(*pendings[HostResolver::kMaxThreads + 1U]);
  // A resolution in progress is not interrupted.
  resolver.cancel(*pendings.front());

  const auto resolved = WaitForResolved(resolver, pendings.size(), 1500ms);
  EXPECT_EQ(resolved.size(), static_cast<std::size_t>(HostResolver::kMaxThreads));
  for (const auto& resolution : resolved) {
    EXPECT_LT(TagOf(resolution), HostResolver::kMaxThreads);
  }
}

TEST(HostResolverTest, DestructionDoesNotWaitForResolutionsInProgress) {
  EventFd wakeupFd;
  test::GetAddrInfoDelayGuard getAddrInfoDelayGuard(1000);
  const auto start = std::chrono::steady_clock::now();
  {
    HostResolver resolver(wakeupFd);
    resolver.resolve(MakeResolution("localhost", 1));
    // Queued behind the first one if a single thread was started, or resolving too: dropped either way.
    resolver.resolve(MakeResolution("localhost", 2));
    std::this_thread::sleep_for(50ms);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 700ms);

  // The resolutions complete after the resolver was destroyed: the event loop is not woken up.
  std::this_thread::sleep_for(1200ms);
  EXPECT_FALSE(IsSignaled(wakeupFd));
}
#endif

}  // namespace aeronet::internal
