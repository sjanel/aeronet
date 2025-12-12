#include "aeronet/request-task.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <coroutine>
#include <memory>
#include <stdexcept>
#include <utility>

namespace aeronet {

namespace {
// Create coroutines that construct a local guard object inside the frame.
// Destroying the frame should run the guard's destructor which flips an atomic.
RequestTask<int> make_guarded_int(std::shared_ptr<std::atomic<int>> alive) {
  struct Guard {
    std::shared_ptr<std::atomic<int>> a;
    Guard(std::shared_ptr<std::atomic<int>> value) : a(std::move(value)) {}
    ~Guard() { a->store(0, std::memory_order_relaxed); }  // NOLINT(modernize-use-equals-default)
  };

  Guard guard(alive);
  co_await std::suspend_always{};  // suspend with Guard alive inside the frame
  co_return 42;
}

RequestTask<void> make_guarded_void(std::shared_ptr<std::atomic<int>> alive) {
  struct Guard {
    std::shared_ptr<std::atomic<int>> a;
    Guard(std::shared_ptr<std::atomic<int>> value) : a(std::move(value)) {}
    ~Guard() { a->store(0, std::memory_order_relaxed); }  // NOLINT(modernize-use-equals-default)
  };

  Guard guard(alive);
  co_await std::suspend_always{};
  co_return;
}

}  // namespace

TEST(RequestTask, ResetDestroysActiveFrame_Value) {
  auto alive = std::make_shared<std::atomic<int>>(1);
  auto task = make_guarded_int(alive);
  EXPECT_TRUE(task.valid());
  // resume once to construct the Guard and reach the suspension point
  task.resume();
  EXPECT_EQ(alive->load(std::memory_order_relaxed), 1);
  task.reset();
  EXPECT_EQ(alive->load(std::memory_order_relaxed), 0);
}

TEST(RequestTask, ResetDestroysActiveFrame_Void) {
  auto alive = std::make_shared<std::atomic<int>>(1);
  auto task = make_guarded_void(alive);
  EXPECT_TRUE(task.valid());
  task.resume();
  EXPECT_EQ(alive->load(std::memory_order_relaxed), 1);
  task.reset();
  EXPECT_EQ(alive->load(std::memory_order_relaxed), 0);
}

namespace {
// A simple coroutine returning int that uses the library's promise_type semantics.
RequestTask<int> make_int_ok() { co_return 7; }

RequestTask<int> make_int_throw() {
  throw std::runtime_error("boom");
  co_return 0;
}

RequestTask<void> make_void_ok() { co_return; }

RequestTask<void> make_void_throw() {
  throw std::runtime_error("void boom");
  co_return;
}

}  // namespace

TEST(RequestTaskPromise, IntSuccessPath) {
  auto task = make_int_ok();
  EXPECT_TRUE(task.valid());
  EXPECT_FALSE(task.done());
  // runSynchronously should finish and return value
  int value = task.runSynchronously();
  EXPECT_EQ(value, 7);
}

TEST(RequestTaskPromise, IntExceptionPath) {
  auto task = make_int_throw();
  EXPECT_TRUE(task.valid());
  // runSynchronously should rethrow the stored exception when consuming
  EXPECT_THROW(task.runSynchronously(), std::runtime_error);
}

TEST(RequestTaskPromise, VoidSuccessPath) {
  auto task = make_void_ok();
  EXPECT_TRUE(task.valid());
  EXPECT_FALSE(task.done());
  // should not throw
  EXPECT_NO_THROW(task.runSynchronously());
}

TEST(RequestTaskPromise, VoidExceptionPath) {
  auto task = make_void_throw();
  EXPECT_TRUE(task.valid());
  // runSynchronously should propagate the exception from promise
  EXPECT_THROW(task.runSynchronously(), std::runtime_error);
}

TEST(RequestTaskExtras, MoveAssignmentAndRelease) {
  // create a suspended task
  auto t1 = make_int_ok();
  EXPECT_TRUE(t1.valid());
  // move-assign into t2
  RequestTask<int> t2;
  t2 = std::move(t1);
  EXPECT_FALSE(t1.valid());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(t2.valid());

  // release returns the handle and leaves task empty
  auto handle = t2.release();
  EXPECT_FALSE(t2.valid());
  if (handle) {
    handle.destroy();
  }
}

TEST(RequestTaskExtras, DestructorCallsResetAndDoneResume) {
  // create a task and advance it to done via runSynchronously
  auto task = make_int_ok();
  EXPECT_TRUE(task.valid());
  EXPECT_FALSE(task.done());
  int val = task.runSynchronously();
  EXPECT_EQ(val, 7);
  EXPECT_TRUE(task.done());
  // reset after done should be safe
  task.reset();
  EXPECT_FALSE(task.valid());
}

TEST(RequestTaskExtras, VoidRunRethrowPath) {
  auto task = make_void_throw();
  EXPECT_TRUE(task.valid());
  EXPECT_THROW(task.runSynchronously(), std::runtime_error);
}

TEST(RequestTaskExtras, MoveAssignmentDestroysPreviousFrame) {
  // Prepare a target that owns an active guarded frame
  auto alive_old = std::make_shared<std::atomic<int>>(1);
  RequestTask<int> target = make_guarded_int(alive_old);
  target.resume();  // guard is now alive in the frame
  EXPECT_TRUE(target.valid());
  EXPECT_EQ(alive_old->load(std::memory_order_relaxed), 1);

  // Prepare a source (will be moved-from)
  RequestTask<int> source = make_int_ok();
  EXPECT_TRUE(source.valid());

  // Move-assign source into target. This should call reset() on target and
  // thus destroy the previous guarded frame (alive_old becomes 0).
  target = std::move(source);
  EXPECT_FALSE(source.valid());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(target.valid());
  EXPECT_EQ(alive_old->load(std::memory_order_relaxed), 0);
}

TEST(RequestTaskExtras, MoveAssignmentDestroysPreviousFrame_Void) {
  // Prepare a target that owns an active guarded void frame
  auto alive_old = std::make_shared<std::atomic<int>>(1);
  RequestTask<void> target = make_guarded_void(alive_old);
  target.resume();  // guard is now alive in the frame
  EXPECT_TRUE(target.valid());
  EXPECT_EQ(alive_old->load(std::memory_order_relaxed), 1);

  // Prepare a source (will be moved-from)
  RequestTask<void> source = make_void_ok();
  EXPECT_TRUE(source.valid());

  auto &sourceRef = source;

  // should do nothing
  source = std::move(sourceRef);
  EXPECT_TRUE(source.valid());

  // Move-assign source into target. This should call reset() on target and
  // thus destroy the previous guarded frame (alive_old becomes 0).
  target = std::move(source);
  EXPECT_FALSE(source.valid());  // NOLINT(bugprone-use-after-move)
  EXPECT_TRUE(target.valid());
  EXPECT_EQ(alive_old->load(std::memory_order_relaxed), 0);
}

TEST(RequestTaskExtras, ReleaseReturnsHandle_ValueAndDestroy) {
  auto alive = std::make_shared<std::atomic<int>>(1);
  RequestTask<int> task = make_guarded_int(alive);
  task.resume();
  EXPECT_TRUE(task.valid());
  auto handle = task.release();
  EXPECT_FALSE(task.valid());
  // destroy manually and ensure guard ran
  if (handle) {
    handle.destroy();
  }
  EXPECT_EQ(alive->load(std::memory_order_relaxed), 0);
}

TEST(RequestTaskExtras, ReleaseReturnsHandle_VoidAndDestroy) {
  auto alive = std::make_shared<std::atomic<int>>(1);
  RequestTask<void> task = make_guarded_void(alive);
  task.resume();
  EXPECT_TRUE(task.valid());
  auto handle = task.release();
  EXPECT_FALSE(task.valid());
  if (handle) {
    handle.destroy();
  }
  EXPECT_EQ(alive->load(std::memory_order_relaxed), 0);
}
}  // namespace aeronet