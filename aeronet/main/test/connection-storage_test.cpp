#include "aeronet/internal/connection-storage.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <utility>

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#endif

#include "aeronet/base-fd.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"

namespace aeronet::internal {

namespace {

// Helper to call recycleOrRelease with proper arguments based on TLS configuration
ConnectionStorage::ConnectionMapIt RecycleConnection(ConnectionStorage& storage, uint32_t maxCached,
                                                     ConnectionStorage::ConnectionMapIt it) {
#ifdef AERONET_ENABLE_OPENSSL
  uint32_t handshakes = 0;
  return storage.recycleOrRelease(maxCached, false, it, handshakes);
#else
  return storage.recycleOrRelease(maxCached, it);
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
  auto [it1, ok1] = storage.emplace(Connection(BaseFd(100)));
  ASSERT_TRUE(ok1);
  auto [it2, ok2] = storage.emplace(Connection(BaseFd(101)));
  ASSERT_TRUE(ok2);
  auto [it3, ok3] = storage.emplace(Connection(BaseFd(102)));
  ASSERT_TRUE(ok3);

  // Set different last activity times
  storage.now = std::chrono::steady_clock::now();
  it1->second->lastActivity = storage.now - std::chrono::hours{2};    // old, should be swept
  it2->second->lastActivity = storage.now - std::chrono::hours{2};    // old, should be swept
  it3->second->lastActivity = storage.now - std::chrono::minutes{5};  // recent, should stay

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

  auto [it1, ok1] = storage.emplace(Connection(BaseFd(200)));
  ASSERT_TRUE(ok1);

  storage.now = std::chrono::steady_clock::now();
  it1->second->lastActivity = storage.now - std::chrono::hours{3};

  RecycleConnection(storage, 10, it1);

  EXPECT_EQ(storage.nbCachedConnections(), 1U);

  // Sweep with 1 hour timeout
  storage.sweepCachedConnections(std::chrono::hours{1});

  EXPECT_EQ(storage.nbCachedConnections(), 0U);
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST(ConnectionStorage, RecycleOrReleaseWithActiveAsyncState) {
  ConnectionStorage storage;

  auto [it, ok] = storage.emplace(Connection(BaseFd(300)));
  ASSERT_TRUE(ok);

  // Create a real coroutine to get a valid handle
  auto coro = makeTestCoroutine();

  // Simulate an active async handler with a coroutine handle
  it->second->asyncState.active = true;
  it->second->asyncState.handle = coro.handle;
  coro.handle = {};  // Transfer ownership to asyncState (clear() will destroy it)

  // Recycle should clear the async state
  RecycleConnection(storage, 10, it);

  EXPECT_EQ(storage.nbCachedConnections(), 1U);
}

TEST(ConnectionStorage, RecycleOrReleaseWithHandleButNotActive) {
  ConnectionStorage storage;

  auto [it, ok] = storage.emplace(Connection(BaseFd(400)));
  ASSERT_TRUE(ok);

  // Create a real coroutine to get a valid handle
  auto coro = makeTestCoroutine();

  // Set handle but not active - this covers the branch: asyncState.handle && !asyncState.active
  it->second->asyncState.handle = coro.handle;
  it->second->asyncState.active = false;  // handle set but not active
  coro.handle = {};                       // Transfer ownership

  // Recycle should clear the async state (covers the || branch)
  RecycleConnection(storage, 10, it);

  EXPECT_EQ(storage.nbCachedConnections(), 1U);
}
#endif

}  // namespace aeronet::internal
