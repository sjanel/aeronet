#include "aeronet/object-pool.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "aeronet/sys_test_support.hpp"

#if AERONET_WANT_MALLOC_OVERRIDES
#include <new>
#endif

namespace aeronet {

namespace {
bool gThrowerDoThrow = false;
}  // namespace

TEST(ObjectPoolTest, TrivialTypeAllocateAndConstruct) {
  ObjectPool<int> pool;

  int *v1 = pool.allocateAndConstruct(10);
  ASSERT_NE(v1, nullptr);
  EXPECT_EQ(*v1, 10);
  EXPECT_EQ(pool.size(), 1U);

  int *v2 = pool.allocateAndConstruct(20);
  ASSERT_NE(v2, nullptr);
  EXPECT_EQ(*v2, 20);
  EXPECT_EQ(pool.size(), 2U);
  // keep v2 alive until end of test
  pool.destroyAndRelease(v2);
  pool.destroyAndRelease(v1);
}

TEST(ObjectPoolTest, ReuseFreelistAfterDestroy) {
  ObjectPool<int> pool;

  int *v1 = pool.allocateAndConstruct(1);
  int *v2 = pool.allocateAndConstruct(2);
  EXPECT_EQ(pool.size(), 2U);
  EXPECT_EQ(*v1, 1);
  EXPECT_EQ(*v2, 2);

  pool.destroyAndRelease(v1);
  EXPECT_EQ(pool.size(), 1U);

  int *v3 = pool.allocateAndConstruct(3);
  ASSERT_NE(v3, nullptr);
  // freed slot should be reused (LIFO) — implementation detail but expected here.
  EXPECT_EQ(*v3, 3);
  EXPECT_EQ(pool.size(), 2U);
}

TEST(ObjectPoolTest, NonTrivialTypeConstructionAndDestroy) {
  ObjectPool<std::string> pool;

  auto *strObj = pool.allocateAndConstruct("hello");
  ASSERT_NE(strObj, nullptr);
  EXPECT_EQ(*strObj, "hello");
  EXPECT_EQ(pool.size(), 1U);

  pool.destroyAndRelease(strObj);
  EXPECT_EQ(pool.size(), 0U);
}

struct Counted {
  static int constructions;
  static int destructions;

  Counted() : value(0) { ++constructions; }
  explicit Counted(int val) : value(val) { ++constructions; }

  ~Counted() { ++destructions; }

  Counted(const Counted &) = delete;
  Counted &operator=(const Counted &) = delete;

  Counted(Counted &&) = delete;
  Counted &operator=(Counted &&) = delete;

  int value;
};

int Counted::constructions = 0;
int Counted::destructions = 0;

TEST(ObjectPoolTest, DestructorsCalledOnPoolDestruction) {
  Counted::constructions = 0;
  Counted::destructions = 0;
  {
    ObjectPool<Counted> pool(3);
    Counted *c1 = pool.allocateAndConstruct(5);
    Counted *c2 = pool.allocateAndConstruct(6);
    Counted *c3 = pool.allocateAndConstruct(7);
    // ensure they are destroyed by pool destructor
    EXPECT_EQ(c1->value, 5);
    EXPECT_EQ(c2->value, 6);
    EXPECT_EQ(c3->value, 7);
    EXPECT_EQ(Counted::constructions, 3);
    EXPECT_EQ(pool.size(), 3U);

    pool.destroyAndRelease(c1);
    EXPECT_EQ(pool.size(), 2U);
    pool.destroyAndRelease(c2);
    EXPECT_EQ(pool.size(), 1U);
  }
  // pool destructor must have destroyed all constructed objects
  EXPECT_EQ(Counted::destructions, 3);
}

TEST(ObjectPoolTest, DestroyReleasesObject) {
  Counted::constructions = 0;
  Counted::destructions = 0;
  ObjectPool<Counted> pool;
  Counted *cptr = pool.allocateAndConstruct(7);
  EXPECT_EQ(Counted::constructions, 1);
  // API requires a non-null pointer and disallows double-destroy; call once
  // and verify the object was destroyed and the pool size updated.
  ASSERT_NE(cptr, nullptr);
  pool.destroyAndRelease(cptr);
  EXPECT_EQ(pool.size(), 0U);
  EXPECT_GE(Counted::destructions, 1);
}

TEST(ObjectPoolTest, VariadicForwardingConstruction) {
  struct Pair {
    int a;
    int b;
    Pair(int aa, int bb) : a(aa), b(bb) {}
  };

  ObjectPool<Pair> pool;
  Pair *pairPtr = pool.allocateAndConstruct(3, 4);
  ASSERT_NE(pairPtr, nullptr);
  EXPECT_EQ(pairPtr->a, 3);
  EXPECT_EQ(pairPtr->b, 4);
}

TEST(ObjectPoolTest, MovePreservesPointersAndValues) {
  ObjectPool<std::string> pool2;
  std::string *origPtr = pool2.allocateAndConstruct("move-me-1");
  std::string *origPtr2 = pool2.allocateAndConstruct("move-me-2");
  ASSERT_NE(origPtr, nullptr);

  ObjectPool<std::string> moved = std::move(pool2);
  // pointer value remains valid (memory not relocated)
  EXPECT_EQ(*origPtr, "move-me-1");
  EXPECT_EQ(*origPtr2, "move-me-2");
  EXPECT_EQ(moved.size(), 2U);
  // we can still destroy via moved pool
  moved.destroyAndRelease(origPtr);
  moved.destroyAndRelease(origPtr2);
  EXPECT_EQ(moved.size(), 0U);
}

TEST(ObjectPoolTest, SelfMoveAssignDoesntDoAnything) {
  ObjectPool<std::string> pool;
  std::string *origPtr = pool.allocateAndConstruct("self-move");
  ASSERT_NE(origPtr, nullptr);

  auto &alias = pool;
  pool = std::move(alias);
  // pointer value remains valid (memory not relocated)
  EXPECT_EQ(*origPtr, "self-move");
  EXPECT_EQ(pool.size(), 1U);
  pool.destroyAndRelease(origPtr);
  EXPECT_EQ(pool.size(), 0U);
}

TEST(ObjectPoolTest, BulkCreateDestroyCheckValues) {
  ObjectPool<int> pool;
  static constexpr int count = 1000;
  std::vector<int *> ptrs;
  ptrs.reserve(static_cast<std::size_t>(count));

  for (int i = 0; i < count; ++i) {
    int *valPtr = pool.allocateAndConstruct(i);
    ASSERT_NE(valPtr, nullptr);
    ptrs.push_back(valPtr);
  }
  EXPECT_EQ(pool.size(), static_cast<std::size_t>(count));

  // destroy even indices
  for (int i = 0; i < count; i += 2) {
    pool.destroyAndRelease(ptrs[static_cast<std::size_t>(i)]);
    ptrs[static_cast<std::size_t>(i)] = nullptr;
  }

  EXPECT_EQ(pool.size(), static_cast<std::size_t>(count / 2));

  // verify remaining values
  for (int i = 1; i < count; i += 2) {
    ASSERT_NE(ptrs[static_cast<std::size_t>(i)], nullptr);
    EXPECT_EQ(*ptrs[static_cast<std::size_t>(i)], i);
    pool.destroyAndRelease(ptrs[static_cast<std::size_t>(i)]);
    ptrs[static_cast<std::size_t>(i)] = nullptr;
  }

  EXPECT_EQ(pool.size(), 0U);
}

TEST(ObjectPoolTest, FuzzAllocFreeCycles) {
  ObjectPool<int> pool;
  constexpr int cycles = 10000;
  std::vector<int *> live;
  live.reserve(1024);

  std::mt19937_64 rng(12345);
  std::uniform_int_distribution<int> op(0, 3);

  for (int i = 0; i < cycles; ++i) {
    int choice = op(rng);
    if (choice != 0 && !live.empty()) {
      // free a random live element
      std::uniform_int_distribution<std::size_t> idx(0, live.size() - 1);
      std::size_t index = idx(rng);
      pool.destroyAndRelease(live[index]);
      live.erase(live.begin() + static_cast<std::vector<int *>::difference_type>(index));
    } else {
      // allocate
      int *val = pool.allocateAndConstruct(i);
      ASSERT_NE(val, nullptr);
      live.push_back(val);
    }
  }

  // verify all live values and clean up
  for (auto &obj : live) {
    EXPECT_GE(*obj, 0);
    pool.destroyAndRelease(obj);
  }

  EXPECT_EQ(pool.size(), 0U);
}

TEST(ObjectPoolTest, FuzzThrowingConstructor) {
  // deterministic RNGs used by the throwing constructor (globals to avoid
  // static members in a local class)
  std::mt19937_64 rng(424242);
  std::uniform_int_distribution<int> dist(0, 99);

  struct ProbThrow {
    ProbThrow(std::mt19937_64 &rng, std::uniform_int_distribution<int> &dist) {
      const int rnd = dist(rng);
      // ~5% chance to throw
      if (rnd < 5) {
        throw std::runtime_error("ctor failed (fuzz)");
      }
      value = std::make_unique<int>(42);
    }
    std::unique_ptr<int> value;
  };

  ObjectPool<ProbThrow> pool;
  constexpr int cycles = 3000;
  std::vector<ProbThrow *> live;
  live.reserve(1024);

  std::uniform_int_distribution<int> op(0, 10);

  int throws = 0;

  for (int i = 0; i < cycles; ++i) {
    int choice = op(rng);
    if (choice == 0 && !live.empty()) {
      std::uniform_int_distribution<std::size_t> idx(0, live.size() - 1);
      std::size_t index = idx(rng);
      // destroy via pool and remove from live list
      ProbThrow *ptr = live[index];
      pool.destroyAndRelease(ptr);
      live.erase(live.begin() + static_cast<std::vector<ProbThrow *>::difference_type>(index));
    } else {
      try {
        ProbThrow *ptr = pool.allocateAndConstruct(rng, dist);
        ASSERT_NE(ptr, nullptr);
        live.push_back(ptr);
      } catch (const std::runtime_error &) {
        // basic guarantee: pool.size() must not have increased
        ++throws;
        // after a throw, verify all live objects still hold valid values
        for (const ProbThrow *pp : live) {
          ASSERT_NE(pp, nullptr);
          ASSERT_NE(pp->value.get(), nullptr);
          EXPECT_EQ(*(pp->value), 42);
        }
      }
    }
  }

  // ensure at least some throws occurred (probabilistic but highly likely)
  EXPECT_GT(throws, 0);

  const auto nbToRemove = pool.size() / 2;
  for (std::size_t i = 0; i < nbToRemove; ++i) {
    pool.destroyAndRelease(live.back());
    live.pop_back();
  }
  EXPECT_EQ(pool.size(), live.size());
}

TEST(ObjectPoolTest, StringStress) {
  ObjectPool<std::string> pool;
  constexpr int countS = 2000;
  std::vector<std::string *> ptrs;
  ptrs.reserve(static_cast<std::size_t>(countS));

  for (int i = 0; i < countS; ++i) {
    std::ostringstream ss;
    ss << "str-" << i << "-" << (i * 17 % 10007);
    std::string str = ss.str();
    auto *val = pool.allocateAndConstruct(str);
    ASSERT_NE(val, nullptr);
    ptrs.push_back(val);
  }

  for (int i = 0; i < countS; ++i) {
    std::ostringstream ss;
    ss << "str-" << i << "-" << (i * 17 % 10007);
    EXPECT_EQ(*ptrs[static_cast<std::size_t>(i)], ss.str());
    pool.destroyAndRelease(ptrs[static_cast<std::size_t>(i)]);
  }

  EXPECT_EQ(pool.size(), 0U);
}

TEST(ObjectPoolTest, DefaultConstructor) {
  ObjectPool<int> pool;
  EXPECT_EQ(pool.size(), 0U);
  EXPECT_EQ(pool.capacity(), 0U);

  int *obj = pool.allocateAndConstruct(99);
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ(*obj, 99);
  EXPECT_EQ(pool.size(), 1U);
  EXPECT_GE(pool.capacity(), 1U);

  pool.destroyAndRelease(obj);
}

TEST(ObjectPoolTest, ClearResetsToInitialCapacityAndAllowsReallocate) {
  constexpr std::size_t initCap = 64;
  ObjectPool<int> pool(initCap);

  // capacity() returns the rounded-up power-of-two initial capacity
  EXPECT_EQ(pool.capacity(), initCap);

  // grow the pool beyond the initial capacity to force several allocations
  std::vector<int *> ptrs;
  ptrs.reserve(initCap * 4);
  for (std::size_t i = 0; i < initCap * 4; ++i) {
    ptrs.push_back(pool.allocateAndConstruct(static_cast<int>(i)));
  }
  EXPECT_GT(pool.capacity(), initCap);

  // clear the pool and ensure no live objects remain
  pool.reset();
  EXPECT_EQ(pool.size(), 0U);

  // allocate again: the pool should recreate blocks starting from the user
  // provided initial capacity
  int *obj = pool.allocateAndConstruct(42);
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ(*obj, 42);
  EXPECT_EQ(pool.capacity(), initCap);

  pool.destroyAndRelease(obj);
}

TEST(ObjectPoolTest, ReleaseMovesValueForTrivialType) {
  ObjectPool<int> pool;

  int *obj = pool.allocateAndConstruct(123);
  ASSERT_NE(obj, nullptr);
  const auto beforeSize = pool.size();

  int v1 = pool.release(obj);
  EXPECT_EQ(v1, 123);
  EXPECT_EQ(pool.size(), beforeSize - 1);

  // capacity remains available and allocations still work after release
  int *obj2 = pool.allocateAndConstruct(456);
  ASSERT_NE(obj2, nullptr);
  EXPECT_EQ(*obj2, 456);
  pool.destroyAndRelease(obj2);
}

TEST(ObjectPoolTest, ReleaseMovesValueForNonTrivialType) {
  ObjectPool<std::string> pool;
  EXPECT_EQ(pool.capacity(), 0);
  std::string *pStr = pool.allocateAndConstruct("hello-release");
  ASSERT_NE(pStr, nullptr);
  const auto beforeSize = pool.size();

  std::string str = pool.release(pStr);
  EXPECT_EQ(str, "hello-release");
  EXPECT_EQ(pool.size(), beforeSize - 1);

  auto *obj = pool.allocateAndConstruct("after-release");
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ(*obj, "after-release");
  pool.destroyAndRelease(obj);
}

TEST(ObjectPoolTest, AllocateAndConstructBasicExceptionGuarantee) {
  struct Thrower {
    Thrower() {
      if (gThrowerDoThrow) {
        throw std::runtime_error("ctor failed");
      }
      value = 7;
    }
    int value;
  };

  ObjectPool<Thrower> pool;

  // first allocation OK
  Thrower *p1 = pool.allocateAndConstruct();
  ASSERT_NE(p1, nullptr);
  EXPECT_EQ(pool.size(), 1U);

  // next allocation will throw
  gThrowerDoThrow = true;
  ASSERT_THROW((void)pool.allocateAndConstruct(), std::runtime_error);

  // pool size must remain unchanged (basic guarantee)
  EXPECT_EQ(pool.size(), 1U);

  // after exception we can still allocate successfully
  gThrowerDoThrow = false;
  Thrower *p2 = pool.allocateAndConstruct();
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(pool.size(), 2U);

  // cleanup
  pool.destroyAndRelease(p2);
  pool.destroyAndRelease(p1);
}

TEST(ObjectPoolTest, ClearPreservesCapacityForInt) {
  constexpr std::size_t initCap = 64;
  ObjectPool<int> pool(initCap);

  // grow the pool a bit
  std::vector<int *> ptrs;
  ptrs.reserve(200);
  for (int i = 0; i < 200; ++i) {
    ptrs.push_back(pool.allocateAndConstruct(i));
  }
  const auto capBefore = pool.capacity();
  EXPECT_GT(capBefore, initCap);

  // clear should destroy live objects but keep capacity
  pool.clear();
  EXPECT_EQ(pool.size(), 0U);
  EXPECT_EQ(pool.capacity(), capBefore);

  // allocations should still work and capacity remains
  int *obj = pool.allocateAndConstruct(42);
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ(*obj, 42);
  EXPECT_EQ(pool.capacity(), capBefore);
  pool.destroyAndRelease(obj);
}

TEST(ObjectPoolTest, ClearPreservesCapacityForString) {
  ObjectPool<std::string> pool;

  std::vector<std::string *> ptrs;
  const auto capacity = 16 + 32 + 64 + 128;
  ptrs.reserve(capacity);
  for (int i = 0; i < capacity; ++i) {
    std::ostringstream ss;
    ss << "s-" << i;
    ptrs.push_back(pool.allocateAndConstruct(ss.str()));
  }

  const auto capBefore = pool.capacity();

  pool.clear();
  EXPECT_EQ(pool.size(), 0U);
  EXPECT_EQ(pool.capacity(), capBefore);

  auto *obj = pool.allocateAndConstruct(std::string("after-clear"));
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ(*obj, "after-clear");
  EXPECT_EQ(pool.capacity(), capBefore);
  pool.destroyAndRelease(obj);

  for (int i = 0; i < capacity; ++i) {
    (void)pool.allocateAndConstruct(std::string("after-clear"));
  }
  EXPECT_EQ(pool.capacity(), capBefore);
}

#if AERONET_WANT_MALLOC_OVERRIDES

TEST(ObjectPoolTest, BasicExceptionGuaranteeOnBlockAllocationFailure) {
  static constexpr std::size_t kInitialCapacity = 64;
  ObjectPool<int> pool(kInitialCapacity);

  // fill the pool to force several block allocations
  std::vector<int *> ptrs;
  ptrs.reserve(kInitialCapacity);
  for (std::size_t i = 0; i < kInitialCapacity; ++i) {
    ptrs.push_back(pool.allocateAndConstruct(i));
  }

  const auto sizeBefore = pool.size();
  const auto capBefore = pool.capacity();

  ASSERT_EQ(sizeBefore, kInitialCapacity);
  ASSERT_EQ(capBefore, kInitialCapacity);

  // next allocation should trigger a new block allocation; fail it
  aeronet::test_support::FailNextMalloc();
  ASSERT_THROW((void)pool.allocateAndConstruct(), std::bad_alloc);

  // basic guarantee: size and capacity remain unchanged
  EXPECT_EQ(pool.size(), sizeBefore);
  EXPECT_EQ(pool.capacity(), capBefore);

  // Check all stored pointers values remain valid
  for (std::size_t i = 0; i < ptrs.size(); ++i) {
    ASSERT_NE(ptrs[i], nullptr);
    EXPECT_EQ(*ptrs[i], static_cast<int>(i));
  }

  // allocations should still work after the failed one
  int *obj = pool.allocateAndConstruct(42);
  ASSERT_NE(obj, nullptr);
  EXPECT_EQ(*obj, 42);
  EXPECT_EQ(pool.size(), sizeBefore + 1U);

  // cleanup
  pool.destroyAndRelease(obj);
  for (auto ptr : ptrs) {
    pool.destroyAndRelease(ptr);
  }
}

#endif

}  // namespace aeronet