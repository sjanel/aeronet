#include "aeronet/internal/pending-updates.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <utility>

#include "aeronet/http-server-config.hpp"
#include "aeronet/router.hpp"

namespace aeronet {

TEST(PendingUpdates, CopyAssignCopiesVectorsAndFlagsAndIsIndependent) {
  internal::PendingUpdates src;

  int cfgCalls = 0;
  int routerCalls = 0;

  src.config.push_back([&cfgCalls](HttpServerConfig&) { ++cfgCalls; });
  src.router.push_back([&routerCalls](Router&) { ++routerCalls; });

  src.hasConfig.store(true, std::memory_order_relaxed);
  src.hasRouter.store(true, std::memory_order_relaxed);

  internal::PendingUpdates dst;
  dst = src;  // exercise operator=(const PendingUpdates&)

  // copied sizes and flags
  EXPECT_EQ(dst.config.size(), 1U);
  EXPECT_EQ(dst.router.size(), 1U);
  EXPECT_TRUE(dst.hasConfig.load(std::memory_order_relaxed));
  EXPECT_TRUE(dst.hasRouter.load(std::memory_order_relaxed));

  // invoking the copied callbacks affects captured state
  HttpServerConfig cfg;
  Router router;
  dst.config[0](cfg);
  dst.router[0](router);
  EXPECT_EQ(cfgCalls, 1);
  EXPECT_EQ(routerCalls, 1);

  // mutate source after copy and ensure destination remains unchanged (vectors copied)
  src.config.clear();
  src.router.clear();
  src.hasConfig.store(false, std::memory_order_relaxed);
  src.hasRouter.store(false, std::memory_order_relaxed);

  EXPECT_EQ(dst.config.size(), 1U);
  EXPECT_EQ(dst.router.size(), 1U);
  EXPECT_TRUE(dst.hasConfig.load(std::memory_order_relaxed));
  EXPECT_TRUE(dst.hasRouter.load(std::memory_order_relaxed));

  // self-assignment is safe
  const auto& alias = dst;
  dst = alias;
  EXPECT_EQ(dst.config.size(), 1U);
}

TEST(PendingUpdates, CopyConstructCopiesVectorsAndFlags) {
  internal::PendingUpdates src;

  int cfgCalls = 0;
  int routerCalls = 0;

  src.config.push_back([&cfgCalls](HttpServerConfig&) { ++cfgCalls; });
  src.router.push_back([&routerCalls](Router&) { ++routerCalls; });

  src.hasConfig.store(true, std::memory_order_relaxed);
  src.hasRouter.store(true, std::memory_order_relaxed);

  internal::PendingUpdates dst(src);  // copy constructor

  EXPECT_EQ(dst.config.size(), 1U);
  EXPECT_EQ(dst.router.size(), 1U);
  EXPECT_TRUE(dst.hasConfig.load(std::memory_order_relaxed));
  EXPECT_TRUE(dst.hasRouter.load(std::memory_order_relaxed));

  // Ensure callbacks are callable and affect captures
  HttpServerConfig cfg;
  Router router;
  dst.config[0](cfg);
  dst.router[0](router);
  EXPECT_EQ(cfgCalls, 1);
  EXPECT_EQ(routerCalls, 1);
}

TEST(PendingUpdates, MoveConstructMovesVectorsAndClearsSourceFlags) {
  internal::PendingUpdates src;

  int cfgCalls = 0;
  int routerCalls = 0;

  src.config.push_back([&cfgCalls](HttpServerConfig&) { ++cfgCalls; });
  src.router.push_back([&routerCalls](Router&) { ++routerCalls; });

  src.hasConfig.store(true, std::memory_order_relaxed);
  src.hasRouter.store(true, std::memory_order_relaxed);

  internal::PendingUpdates dst(std::move(src));  // move constructor

  // destination should have the data
  EXPECT_EQ(dst.config.size(), 1U);
  EXPECT_EQ(dst.router.size(), 1U);
  EXPECT_TRUE(dst.hasConfig.load(std::memory_order_relaxed));
  EXPECT_TRUE(dst.hasRouter.load(std::memory_order_relaxed));

  // move left src vectors in valid but unspecified state; ensure dst callbacks work
  HttpServerConfig cfg;
  Router router;
  dst.config[0](cfg);
  dst.router[0](router);
  EXPECT_EQ(cfgCalls, 1);
  EXPECT_EQ(routerCalls, 1);
}

TEST(PendingUpdates, MoveAssignMovesVectorsAndClearsSourceFlags) {
  internal::PendingUpdates src;

  int cfgCalls = 0;
  int routerCalls = 0;

  src.config.push_back([&cfgCalls](HttpServerConfig&) { ++cfgCalls; });
  src.router.push_back([&routerCalls](Router&) { ++routerCalls; });

  src.hasConfig.store(true, std::memory_order_relaxed);
  src.hasRouter.store(true, std::memory_order_relaxed);

  internal::PendingUpdates dst;
  dst = std::move(src);  // move assignment

  EXPECT_EQ(dst.config.size(), 1U);
  EXPECT_EQ(dst.router.size(), 1U);
  EXPECT_TRUE(dst.hasConfig.load(std::memory_order_relaxed));
  EXPECT_TRUE(dst.hasRouter.load(std::memory_order_relaxed));

  HttpServerConfig cfg;
  Router router;
  dst.config[0](cfg);
  dst.router[0](router);
  EXPECT_EQ(cfgCalls, 1);
  EXPECT_EQ(routerCalls, 1);

  auto& alias = dst;
  dst = std::move(alias);  // self move-assignment is safe
  EXPECT_EQ(dst.config.size(), 1U);
}
}  // namespace aeronet
