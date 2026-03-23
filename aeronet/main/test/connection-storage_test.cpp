#include "aeronet/internal/connection-storage.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#endif

#include "aeronet/base-fd.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"

#ifdef AERONET_POSIX
#include "aeronet/native-handle.hpp"
#endif

namespace aeronet::internal {

namespace {

// Helper to call recycleOrRelease with proper arguments based on TLS configuration
void RecycleConnection(ConnectionStorage& storage, uint32_t maxCached, ConnectionStorage::ConnectionIt it) {
#ifdef AERONET_ENABLE_OPENSSL
  uint32_t handshakes = 0;
  storage.recycleOrRelease(it, maxCached, false, handshakes);
#else
  storage.recycleOrRelease(it, maxCached);
#endif
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
// A minimal coroutine that creates a proper coroutine frame for testing
struct TestCoroutine {
  struct promise_type {
    TestCoroutine get_return_object() {
      return TestCoroutine{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    std::suspend_always initial_suspend() noexcept { return {}; }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    std::suspend_always final_suspend() noexcept { return {}; }

    void return_void() {}
    void unhandled_exception() {}
  };

  std::coroutine_handle<promise_type> handle;

  explicit TestCoroutine(std::coroutine_handle<promise_type> handle) : handle(handle) {}

  TestCoroutine(const TestCoroutine&) = delete;
  TestCoroutine& operator=(const TestCoroutine&) = delete;
  TestCoroutine(TestCoroutine&& other) noexcept : handle(std::exchange(other.handle, {})) {}
  TestCoroutine& operator=(TestCoroutine&& other) noexcept {
    if (this != &other) {
      if (handle) {
        handle.destroy();
      }
      handle = std::exchange(other.handle, {});
    }
    return *this;
  }
  ~TestCoroutine() {
    if (handle) {
      handle.destroy();
    }
  }
};

TestCoroutine makeTestCoroutine() { co_return; }
#endif

}  // namespace

TEST(ConnectionStorage, SweepCachedConnectionsRemovesExpired) {
  ConnectionStorage storage;

  // Create connections and then recycle them to populate the cache
  auto it1 = storage.emplace(Connection(BaseFd(100)));
  ASSERT_TRUE(it1 != storage.end());
  auto it2 = storage.emplace(Connection(BaseFd(101)));
  ASSERT_TRUE(it2 != storage.end());
  auto it3 = storage.emplace(Connection(BaseFd(102)));
  ASSERT_TRUE(it3 != storage.end());

  // Set different last activity times
  storage.now = std::chrono::steady_clock::now();
  storage.connectionState(it1).lastActivity = storage.now - std::chrono::hours{2};    // old, should be swept
  storage.connectionState(it2).lastActivity = storage.now - std::chrono::hours{2};    // old, should be swept
  storage.connectionState(it3).lastActivity = storage.now - std::chrono::minutes{5};  // recent, should stay

  // Recycle all connections (adds them to cache)
  RecycleConnection(storage, 10, it1);
  RecycleConnection(storage, 10, it2);
  RecycleConnection(storage, 10, it3);

  EXPECT_EQ(storage.nbCachedConnections(), 3U);

  // Sweep with 1 hour timeout - should remove first two
  storage.sweepCachedConnections(std::chrono::hours{1});

  EXPECT_EQ(storage.nbCachedConnections(), 1U);
}

TEST(ConnectionStorage, SweepCachedConnectionsRemovesAll) {
  ConnectionStorage storage;

  auto it1 = storage.emplace(Connection(BaseFd(200)));
  ASSERT_TRUE(it1 != storage.end());

  storage.now = std::chrono::steady_clock::now();
  storage.connectionState(it1).lastActivity = storage.now - std::chrono::hours{3};

  RecycleConnection(storage, 10, it1);

  EXPECT_EQ(storage.nbCachedConnections(), 1U);

  // Sweep with 1 hour timeout
  storage.sweepCachedConnections(std::chrono::hours{1});

  EXPECT_EQ(storage.nbCachedConnections(), 0U);
}

#ifdef AERONET_POSIX
TEST(ConnectionStorage, ShrinkToFitTrimsTrailingNulls) {
  ConnectionStorage storage;

  // Create 10 connections
  for (NativeHandle fd = 1; fd <= 10; ++fd) {
    auto it = storage.emplace(Connection(BaseFd(10 + fd)));
    ASSERT_TRUE(it != storage.end());
  }

  // Recycle last 3 connections (make their states nullptr)
  for (NativeHandle fd = 8; fd <= 10; ++fd) {
    RecycleConnection(storage, 10, storage.iterator(10 + fd));
  }

  // Before shrinking, vector capacity remains at least 10 entries
  // After shrink, we expect the vectors trimmed to the last non-null index (7 entries)
  storage.shrink_to_fit();

  EXPECT_EQ(storage.size(), 7U);
  EXPECT_FALSE(storage.empty());
}

TEST(ConnectionStorage, ShrinkToFitShrinksLargeCapacity) {
  ConnectionStorage storage;

  // Create a large number of connections to grow internal vectors' capacity
  const int total = 800;
  for (int i = 0; i < total; ++i) {
    auto it = storage.emplace(Connection(BaseFd(i + 10)));
    ASSERT_TRUE(it != storage.end());
  }

  // Keep first 129 active, recycle the rest to create trailing nulls
  const int keep = 129;
  for (int i = keep; i < total; ++i) {
    RecycleConnection(storage, 0xFFFFFFFF, storage.iterator(i + 10));
  }

  // Sanity: nb active should equal 'keep'
  EXPECT_EQ(storage.size(), static_cast<std::size_t>(keep));

  // Now call shrink_to_fit which should erase trailing slots and trigger capacity shrink branch
  storage.shrink_to_fit();

  // POSIX only: verify trailing null vector slots were trimmed (maps have no trailing slots).
  EXPECT_LT(storage.end() - storage.begin(), total);
  EXPECT_EQ(storage.size(), static_cast<std::size_t>(keep));
}

TEST(ConnectionStorage, ShrinkToFitDoesNotShrinkSmallEmptyCapacity) {
  ConnectionStorage storage;

  // Create a large number of connections to grow internal vectors' capacity
  const int total = 140;
  for (int i = 0; i < total; ++i) {
    auto it = storage.emplace(Connection(BaseFd(i + 10)));
    ASSERT_TRUE(it != storage.end());
  }

  // Keep first 130 active, recycle the rest to create trailing nulls
  const int keep = 130;
  for (int i = keep; i < total; ++i) {
    RecycleConnection(storage, 0xFFFFFFFF, storage.iterator(i + 10));
  }

  // Sanity: nb active should equal 'keep'
  EXPECT_EQ(storage.size(), static_cast<std::size_t>(keep));

  // Now call shrink_to_fit which should erase trailing slots and trigger capacity shrink branch
  storage.shrink_to_fit();

  // POSIX only: verify trailing null vector slots were trimmed (maps have no trailing slots).
  EXPECT_LT(storage.end() - storage.begin(), total);
  EXPECT_EQ(storage.size(), static_cast<std::size_t>(keep));
}
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST(ConnectionStorage, RecycleOrReleaseWithActiveAsyncState) {
  ConnectionStorage storage;

  auto it = storage.emplace(Connection(BaseFd(300)));
  ASSERT_TRUE(it != storage.end());

  // Create a real coroutine to get a valid handle
  auto coro = makeTestCoroutine();

  // Simulate an active async handler with a coroutine handle
  storage.connectionState(it).asyncState.active = true;
  storage.connectionState(it).asyncState.handle = coro.handle;
  coro.handle = {};  // Transfer ownership to asyncState (clear() will destroy it)

  // Recycle should clear the async state
  RecycleConnection(storage, 10, it);

  EXPECT_EQ(storage.nbCachedConnections(), 1U);
}

TEST(ConnectionStorage, RecycleOrReleaseWithHandleButNotActive) {
  ConnectionStorage storage;

  auto it = storage.emplace(Connection(BaseFd(400)));
  ASSERT_TRUE(it != storage.end());

  // Create a real coroutine to get a valid handle
  auto coro = makeTestCoroutine();

  // Set handle but not active - this covers the branch: asyncState.handle && !asyncState.active
  storage.connectionState(it).asyncState.handle = coro.handle;
  storage.connectionState(it).asyncState.active = false;  // handle set but not active
  coro.handle = {};                                       // Transfer ownership

  // Recycle should clear the async state (covers the || branch)
  RecycleConnection(storage, 10, it);

  EXPECT_EQ(storage.nbCachedConnections(), 1U);
}
#endif

}  // namespace aeronet::internal
