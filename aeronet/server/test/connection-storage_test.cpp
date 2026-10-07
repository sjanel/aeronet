#include "aeronet/internal/connection-storage.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#endif

#include "aeronet/base-fd.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/async-handler-state.hpp"
#include "aeronet/protocol-handler.hpp"
#endif

#ifdef AERONET_POSIX
#include "aeronet/http-request-view.hpp"
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

// Protocol handler reporting background work in flight until told otherwise (dropAsyncTask() is not overridden).
class AsyncWorkProtocolHandler final : public IProtocolHandler {
 public:
  [[nodiscard]] ProtocolType type() const noexcept override { return ProtocolType::Http2; }

  [[nodiscard]] ProtocolProcessResult processInput(std::span<const std::byte> /*data*/,
                                                   ConnectionState& /*state*/) override {
    return {};
  }

  [[nodiscard]] std::span<const std::byte> getPendingOutput() const noexcept override { return {}; }

  void onOutputWritten(std::size_t /*bytesWritten*/) override {}

  void onTransportClosing() override {}

  [[nodiscard]] bool hasAsyncWorkInFlight() const noexcept override { return workInFlight; }

  bool workInFlight{true};
};
#endif

}  // namespace

TEST(ConnectionStorage, ShrinkToFitOnEmptyStorageIsNoOp) {
  ConnectionStorage storage;

  storage.shrink_to_fit();

  EXPECT_TRUE(storage.empty());
  EXPECT_EQ(storage.size(), 0U);
}

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
TEST(ConnectionStorage, ConnectionStateAddressSurvivesFdVectorGrowth) {
  ConnectionStorage storage;

  constexpr NativeHandle firstFd = 100;
  auto firstIt = storage.emplace(Connection(BaseFd(firstFd)));
  ConnectionState* const pFirstState = storage.pConnectionState(firstIt);
  HttpRequestView* const pFirstRequest = &pFirstState->request;

  // Force both fd-indexed vectors to grow. Async coroutine frames may retain pFirstRequest, so neither address may
  // change when a later accept or CONNECT insertion uses a higher fd.
  constexpr NativeHandle highFd = 4096;
  ASSERT_TRUE(storage.emplace(Connection(BaseFd(highFd))) != storage.end());

  EXPECT_EQ(storage.pConnectionState(firstFd), pFirstState);
  EXPECT_EQ(&storage.connectionState(storage.iterator(firstFd)).request, pFirstRequest);
}

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

// macOS only: this reproduces the out-of-bounds read behind the crash observed there during graceful
// drain, where a late kqueue event references a connection whose trailing vector slot shrink_to_fit()
// has already trimmed. On macOS iterator(fd) must map such an out-of-range fd to end() so IsValid()
// reports it as gone, instead of computing begin() + (fd - 1) and letting IsValid dereference past the
// vector. Linux epoll never delivers such a stale fd, so the guard (and this test) are macOS-only.
#ifdef AERONET_MACOS
TEST(ConnectionStorage, StaleFdBeyondShrunkVectorIsReportedGone) {
  ConnectionStorage storage;

  // Emplace 10 connections; the vector is indexed by (fd - 1), so it grows to hold fd 110.
  for (NativeHandle fd = 101; fd <= 110; ++fd) {
    ASSERT_TRUE(storage.emplace(Connection(BaseFd(fd))) != storage.end());
  }

  // Close the three highest fds and trim the trailing null slots: the vector now ends at fd 107.
  for (NativeHandle fd = 108; fd <= 110; ++fd) {
    RecycleConnection(storage, 10, storage.iterator(fd));
  }
  storage.shrink_to_fit();
  ASSERT_EQ(storage.end() - storage.begin(), 107);

  // A live, in-range fd stays valid.
  EXPECT_TRUE(IsValid(storage, storage.iterator(107)));

  // A dead but in-range fd (slot present yet empty) is reported gone via the emptiness check.
  EXPECT_FALSE(IsValid(storage, storage.iterator(50)));

  // Stale fds whose (fd - 1) index now lands at/after the shrunk vector end must be reported gone;
  // previously IsValid dereferenced past the vector here (out-of-bounds read / operator[] assertion).
  for (NativeHandle staleFd = 108; staleFd <= 115; ++staleFd) {
    EXPECT_FALSE(IsValid(storage, storage.iterator(staleFd))) << "stale fd " << staleFd;
  }

  // Invalid / non-positive fds wrap to a huge index and are likewise reported gone.
  EXPECT_FALSE(IsValid(storage, storage.iterator(0)));
  EXPECT_FALSE(IsValid(storage, storage.iterator(kInvalidHandle)));
}
#endif  // AERONET_MACOS

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
  auto& state = storage.connectionState(it);
  (void)state.ensureAsyncState(storage.asyncHandlerStatePool());

  // Simulate an active async handler with a coroutine handle
  state.asyncState->active = true;
  state.asyncState->handle = coro.handle;
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
  auto& state = storage.connectionState(it);
  (void)state.ensureAsyncState(storage.asyncHandlerStatePool());

  // Set handle but not active - this covers the branch: asyncState.handle && !asyncState.active
  state.asyncState->handle = coro.handle;
  state.asyncState->active = false;  // handle set but not active
  coro.handle = {};                  // Transfer ownership

  // Recycle should clear the async state (covers the || branch)
  RecycleConnection(storage, 10, it);

  EXPECT_EQ(storage.nbCachedConnections(), 1U);
}

TEST(ConnectionStorage, FindConnectionChecksFdAndGeneration) {
  ConnectionStorage storage;

  auto it = storage.emplace(Connection(BaseFd(700)));
  const uint32_t generation = storage.connectionState(it).generation;

  EXPECT_TRUE(storage.findConnection(700, generation) == it);
  // Same fd, another connection.
  EXPECT_TRUE(storage.findConnection(700, generation + 1) == storage.end());
  // Fd never used.
  EXPECT_TRUE(storage.findConnection(5000, generation) == storage.end());

  RecycleConnection(storage, 10, it);
  EXPECT_TRUE(storage.findConnection(700, generation) == storage.end());
}

TEST(ConnectionStorage, ClosedConnectionIsKeptUntilItsDeferredWorkCompletes) {
  ConnectionStorage storage;

  auto it = storage.emplace(Connection(BaseFd(500)));
  auto coro = makeTestCoroutine();
  const std::coroutine_handle<> handle = coro.handle;
  auto& state = storage.connectionState(it);
  const uint32_t generation = state.generation;
  auto& asyncState = state.ensureAsyncState(storage.asyncHandlerStatePool());
  asyncState.active = true;
  asyncState.handle = coro.handle;
  asyncState.awaitReason = AsyncHandlerState::AwaitReason::WaitingForCallback;
  coro.handle = {};  // Transfer ownership

  // The coroutine waits for deferred work, which may use it: the state is not reused.
  RecycleConnection(storage, 10, it);
  EXPECT_EQ(storage.nbCachedConnections(), 0U);
  EXPECT_EQ(storage.nbOrphanedConnectionStates(), 1U);

  // Completion of another connection.
  storage.releaseOrphanedAsyncTask(generation + 1, handle, 10);
  EXPECT_EQ(storage.nbOrphanedConnectionStates(), 1U);

  storage.releaseOrphanedAsyncTask(generation, handle, 10);
  EXPECT_FALSE(storage.hasOrphanedConnectionStates());
  EXPECT_EQ(storage.nbCachedConnections(), 1U);
}

TEST(ConnectionStorage, ClosedConnectionIsKeptUntilAllItsDeferredWorkCompleted) {
  ConnectionStorage storage;

  auto it = storage.emplace(Connection(BaseFd(600)));
  auto& state = storage.connectionState(it);
  const uint32_t generation = state.generation;
  auto handler = std::make_unique<AsyncWorkProtocolHandler>();
  AsyncWorkProtocolHandler* pHandler = handler.get();
  state.protocolHandler = std::move(handler);

  RecycleConnection(storage, 10, it);
  EXPECT_EQ(storage.nbOrphanedConnectionStates(), 1U);

  // One work completed, another one still runs.
  storage.releaseOrphanedAsyncTask(generation, std::coroutine_handle<>{}, 10);
  EXPECT_EQ(storage.nbOrphanedConnectionStates(), 1U);

  pHandler->workInFlight = false;
  storage.releaseOrphanedAsyncTask(generation, std::coroutine_handle<>{}, 10);
  EXPECT_FALSE(storage.hasOrphanedConnectionStates());
  EXPECT_EQ(storage.nbCachedConnections(), 1U);
}
#endif

}  // namespace aeronet::internal
