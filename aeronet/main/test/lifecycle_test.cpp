#include "aeronet/internal/lifecycle.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <utility>

namespace aeronet::internal {

TEST(LifecycleTest, MoveConstructorCopiesState) {
  Lifecycle original;
  original.enterRunning();
  original.drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  original.drainDeadlineEnabled = true;

  Lifecycle moved(std::move(original));

  EXPECT_EQ(moved.state.load(), Lifecycle::State::Running);
  EXPECT_TRUE(moved.drainDeadlineEnabled);
  EXPECT_NE(moved.drainDeadline.time_since_epoch().count(), 0);
}

TEST(LifecycleTest, MoveAssignmentCopiesState) {
  Lifecycle original;
  original.enterRunning();
  original.drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  original.drainDeadlineEnabled = true;

  Lifecycle moved;
  moved = std::move(original);

  EXPECT_EQ(moved.state.load(), Lifecycle::State::Running);
  EXPECT_TRUE(moved.drainDeadlineEnabled);
  EXPECT_NE(moved.drainDeadline.time_since_epoch().count(), 0);
}

TEST(LifecycleTest, ResetClearsState) {
  Lifecycle lifecycle;
  lifecycle.enterRunning();
  lifecycle.drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  lifecycle.drainDeadlineEnabled = true;

  lifecycle.reset();

  EXPECT_EQ(lifecycle.state.load(), Lifecycle::State::Idle);
  EXPECT_FALSE(lifecycle.drainDeadlineEnabled);
  EXPECT_EQ(lifecycle.drainDeadline.time_since_epoch().count(), 0);
  EXPECT_FALSE(lifecycle.started.load());
  EXPECT_FALSE(lifecycle.ready.load());
}

TEST(LifecycleTest, SelfMoveAssignmentIsNoop) {
  Lifecycle lifecycle;
  lifecycle.enterRunning();
  lifecycle.drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  lifecycle.drainDeadlineEnabled = true;

  auto &self = lifecycle;

  lifecycle = std::move(self);

  EXPECT_EQ(lifecycle.state.load(), Lifecycle::State::Running);
  EXPECT_TRUE(lifecycle.drainDeadlineEnabled);
  EXPECT_NE(lifecycle.drainDeadline.time_since_epoch().count(), 0);
  EXPECT_TRUE(lifecycle.started.load());
  EXPECT_TRUE(lifecycle.ready.load());
}

TEST(LifecycleTest, ShrinkDeadlineUpdatesDeadline) {
  Lifecycle lifecycle;
  lifecycle.enterDraining(std::chrono::steady_clock::now() + std::chrono::seconds(10), true);

  auto newDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  lifecycle.shrinkDeadline(newDeadline);

  EXPECT_EQ(lifecycle.drainDeadline, newDeadline);
}

TEST(LifecycleTest, ShrinkDeadlineDoesNotUpdateIfLater) {
  Lifecycle lifecycle;
  auto originalDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  lifecycle.enterDraining(originalDeadline, true);

  auto laterDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  lifecycle.shrinkDeadline(laterDeadline);

  EXPECT_EQ(lifecycle.drainDeadline, originalDeadline);
}

}  // namespace aeronet::internal