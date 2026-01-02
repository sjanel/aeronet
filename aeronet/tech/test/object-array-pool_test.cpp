#include "aeronet/object-array-pool.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/sys-test-support.hpp"

#if AERONET_WANT_MALLOC_OVERRIDES
#include <new>
#endif

namespace aeronet {

TEST(ObjectArrayPoolTest, DefaultConstructor) {
  ObjectArrayPool<char> pool;

  EXPECT_EQ(pool.capacity(), 0U);
  pool.clear();
  EXPECT_EQ(pool.capacity(), 0U);
  pool.reset();
  EXPECT_EQ(pool.capacity(), 0U);
}

#if AERONET_WANT_MALLOC_OVERRIDES

TEST(ObjectArrayPoolTest, AllocateFailsToAllocateMemory) {
  ObjectArrayPool<std::string> pool;

  test::FailNextMalloc();  // cause next malloc to fail

  // Allocation should throw std::bad_alloc
  EXPECT_THROW((void)pool.allocateAndDefaultConstruct(5), std::bad_alloc);
}

#endif

TEST(ObjectArrayPoolCharTest, AllocateZeroLengthAndValueConstruct) {
  ObjectArrayPool<char> pool;

  // zero-length allocate should return non-null and count as a live allocation
  char *zero1 = pool.allocateAndDefaultConstruct(0);
  ASSERT_NE(zero1, nullptr);

  // zero-length value-construct should also return non-null
  char *zero2 = pool.allocateAndDefaultConstruct(0);
  ASSERT_NE(zero2, nullptr);

  // allocate a small array and verify default-initialization for value-construct
  char *arr = pool.allocateAndDefaultConstruct(4);
  ASSERT_NE(arr, nullptr);
  arr[0] = 'A';
  arr[1] = 'B';
  arr[2] = 'C';
  arr[3] = 'D';

  std::string_view strView(arr, 4);

  // assign and verify values
  EXPECT_EQ(strView, "ABCD");
}

TEST(ObjectArrayPoolStringTest, AllocateAssignAndClearReset) {
  ObjectArrayPool<std::string> pool(8);

  // allocate a few arrays of strings and assign values
  std::string *s1 = pool.allocateAndDefaultConstruct(3);
  std::string *s2 = pool.allocateAndDefaultConstruct(2);
  ASSERT_NE(s1, nullptr);
  ASSERT_NE(s2, nullptr);

  s1[0] = "one";
  s1[1] = "two";
  s1[2] = "three";

  s2[0] = "x";
  s2[1] = "y";

  // capacity should be at least the initial capacity provided
  EXPECT_GE(pool.capacity(), static_cast<ObjectArrayPool<std::string>::size_type>(8));

  // clear destroys live objects but preserves capacity
  auto capBefore = pool.capacity();
  pool.clear();
  EXPECT_EQ(pool.capacity(), capBefore);

  // allocate again after clear
  auto *s3 = pool.allocateAndDefaultConstruct(1);
  ASSERT_NE(s3, nullptr);
  s3[0] = "again";
  EXPECT_EQ(s3[0], "again");

  // reset releases all memory and capacity becomes zero
  pool.reset();
  EXPECT_EQ(pool.capacity(), 0U);
}

TEST(ObjectArrayPoolBothTypesTest, MovePreservesPointersAndValues) {
  // Test with char (trivial)
  ObjectArrayPool<char> charPool;
  char *carr = charPool.allocateAndDefaultConstruct(3);
  ASSERT_NE(carr, nullptr);
  carr[0] = 'a';
  carr[1] = 'b';

  ObjectArrayPool<char> movedChar = std::move(charPool);
  EXPECT_EQ(carr[0], 'a');
  EXPECT_EQ(carr[1], 'b');

  // Test with std::string (non-trivial)
  ObjectArrayPool<std::string> strPool;
  std::string *sarr = strPool.allocateAndDefaultConstruct(2);
  ASSERT_NE(sarr, nullptr);
  sarr[0] = "hello";
  sarr[1] = "world";

  ObjectArrayPool<std::string> movedStr = std::move(strPool);
  EXPECT_EQ(sarr[0], "hello");
  EXPECT_EQ(sarr[1], "world");

  movedStr.clear();
}

TEST(ObjectArrayPoolMoveTest, MoveConstructorMultipleBlocks) {
  // Force multiple blocks by using a small initial capacity and allocating more.
  ObjectArrayPool<std::string> src(4);

  std::string *arr = src.allocateAndDefaultConstruct(3);
  arr[0] = "one";
  arr[1] = "two";
  arr[2] = "three";

  // Allocate another array to push into the next block
  std::string *arr2 = src.allocateAndDefaultConstruct(4);
  for (int i = 0; i < 4; ++i) {
    arr2[i] = "b" + std::to_string(i);
  }

  const auto capBefore = src.capacity();

  // Move-construct
  ObjectArrayPool<std::string> moved = std::move(src);

  // Moved pool should preserve capacity and values
  EXPECT_GE(moved.capacity(), capBefore);
  EXPECT_EQ(arr[0], "one");
  EXPECT_EQ(arr[1], "two");
  EXPECT_EQ(arr[2], "three");
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(arr2[i], "b" + std::to_string(i));
  }
}

TEST(ObjectArrayPoolMoveTest, MoveAssignmentOverExistingPool) {
  // Destination has some allocations that should be released when move-assigned into.
  ObjectArrayPool<char> dest;
  char *d0 = dest.allocateAndDefaultConstruct(8);
  d0[0] = 'x';

  ObjectArrayPool<char> src;
  char *s0 = src.allocateAndDefaultConstruct(3);
  s0[0] = 'a';
  s0[1] = 'b';

  const auto srcCap = src.capacity();

  dest = std::move(src);

  // dest should now have the content formerly in src
  EXPECT_GE(dest.capacity(), srcCap);
  EXPECT_EQ(s0[0], 'a');
  EXPECT_EQ(s0[1], 'b');

  const auto oldCapa = dest.capacity();

  auto &alias = dest;
  dest = std::move(alias);

  EXPECT_EQ(dest.capacity(), oldCapa);

  EXPECT_EQ(s0[0], 'a');
  EXPECT_EQ(s0[1], 'b');
}

TEST(ObjectArrayPoolStressTest, BulkAllocateAndReset) {
  ObjectArrayPool<char> pool;

  std::vector<char *> allocated;
  for (int i = 0; i < 128; ++i) {
    char *arr = pool.allocateAndDefaultConstruct(8);
    ASSERT_NE(arr, nullptr);
    arr[0] = static_cast<char>(i);
    allocated.push_back(arr);
  }

  EXPECT_GT(pool.capacity(), 0U);

  pool.reset();
  EXPECT_EQ(pool.capacity(), 0U);
}

TEST(ObjectArrayPoolShrinkTest, TrivialTypeShrinkAndReuse) {
  ObjectArrayPool<char> pool;
  char *arr = pool.allocateAndDefaultConstruct(5);
  ASSERT_NE(arr, nullptr);

  // shrink the last allocated array to size 3 IMMEDIATELY after allocation
  pool.shrinkLastAllocated(arr, 3);

  // next allocation of size 2 should reuse the freed tail of the previous array
  char *arr2 = pool.allocateAndDefaultConstruct(2);
  ASSERT_NE(arr2, nullptr);
  EXPECT_EQ(arr2, arr + 3);

  // write after allocations
  for (int i = 0; i < 3; ++i) {
    arr[i] = static_cast<char>(i + 1);
  }
  arr2[0] = 42;
  arr2[1] = 43;
  EXPECT_EQ(arr[3], 42);
  EXPECT_EQ(arr[4], 43);
}

TEST(ObjectArrayPoolShrinkTest, NonTrivialTypeDestroyThenShrink) {
  ObjectArrayPool<std::string> pool;
  std::string *arr = pool.allocateAndDefaultConstruct(3);
  ASSERT_NE(arr, nullptr);

  // shrink must be called immediately after allocation; do that now.
  pool.shrinkLastAllocated(arr, 1);

  // Now allocate the tail and use it.
  std::string *arr2 = pool.allocateAndDefaultConstruct(2);
  ASSERT_NE(arr2, nullptr);
  EXPECT_EQ(arr2, arr + 1);

  // assign values after allocation/shrink
  arr[0] = "keep";
  arr2[0] = "new1";
  arr2[1] = "new2";
  EXPECT_EQ(arr[0], "keep");
  EXPECT_EQ(arr2[0], "new1");
  EXPECT_EQ(arr2[1], "new2");
}

TEST(ObjectArrayPoolTest, ShouldReuseNextBlocksAfterClear) {
  // Construct a pool with small initial capacity so multiple blocks are created.
  ObjectArrayPool<std::string> pool(4);

  for (uint32_t i = 0; i < 128U; ++i) {
    std::string *arr = pool.allocateAndDefaultConstruct(i);
    ASSERT_NE(arr, nullptr);
    for (uint32_t j = 0; j < i; ++j) {
      arr[j] = "A-very-long-string-to-avoid-sso-" + std::to_string(j);
    }
  }

  const auto capacity = pool.capacity();

  pool.clear();

  for (uint32_t i = 0; i < 64U; ++i) {
    std::string *arr = pool.allocateAndDefaultConstruct(i);
    ASSERT_NE(arr, nullptr);
    for (uint32_t j = 0; j < i; ++j) {
      arr[j] = "A-very-long-string-to-avoid-sso-" + std::to_string(j);
    }
  }

  EXPECT_EQ(pool.capacity(), capacity);
}

TEST(ObjectArrayPoolReuseNextBlockAfterClear, TrivialType) {
  // Construct a pool with small initial capacity so multiple blocks are created.
  ObjectArrayPool<char> pool(4);

  // Fill first block completely to force creation of a second block.
  char *a1 = pool.allocateAndDefaultConstruct(4);
  ASSERT_NE(a1, nullptr);

  // This allocation will be at the start of the second block.
  char *secondStart = pool.allocateAndDefaultConstruct(1);
  ASSERT_NE(secondStart, nullptr);

  auto capBefore = pool.capacity();

  // Clear should reset allocation cursor to first block.
  pool.clear();

  // Allocate an array that fits into the existing second block.
  char *reused = pool.allocateAndDefaultConstruct(5);
  ASSERT_NE(reused, nullptr);

  // The allocator should have switched to the existing second block rather than allocating a new one.
  EXPECT_EQ(reused, secondStart);
  EXPECT_EQ(pool.capacity(), capBefore);
}

TEST(ObjectArrayPoolReuseNextBlockAfterClear, NonTrivialType) {
  ObjectArrayPool<std::string> pool(3);

  std::string *a1 = pool.allocateAndDefaultConstruct(4);
  ASSERT_NE(a1, nullptr);

  *a1 = "first-block-end";
  *(a1 + 1) = "first-block-end-2";
  *(a1 + 2) = "first-block-end-3";
  *(a1 + 3) = "first-block-end-4";

  std::string *secondStart = pool.allocateAndDefaultConstruct(1);
  ASSERT_NE(secondStart, nullptr);

  *secondStart = "second-block-start";

  EXPECT_EQ(*a1, "first-block-end");
  EXPECT_EQ(*(a1 + 1), "first-block-end-2");
  EXPECT_EQ(*(a1 + 2), "first-block-end-3");
  EXPECT_EQ(*(a1 + 3), "first-block-end-4");
  EXPECT_EQ(*secondStart, "second-block-start");

  // clear destroys constructed objects and rewinds to first block
  pool.clear();

  std::string *reused = pool.allocateAndDefaultConstruct(16);
  for (std::size_t i = 0; i < 16; ++i) {
    reused[i] = "reused-" + std::to_string(i);
  }
  ASSERT_NE(reused, nullptr);

  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(reused[i], "reused-" + std::to_string(i));
  }
}

// Fuzz-style tests: perform random sequences of operations on the pool to
// exercise corner cases around allocation, shrinking and clearing. These are
// deterministic by seeding the RNG so they are reproducible in CI.
namespace {
void RunFuzzPoolChar(unsigned seed) {
  std::mt19937_64 rng(seed);
  aeronet::ObjectArrayPool<char> pool(8);
  std::vector<std::pair<char *, size_t>> allocated;
  bool lastWasAlloc = false;

  for (int op = 0; op < 1000; ++op) {
    int choice = rng() % 4;
    if (choice == 0) {
      // allocate a random small array
      size_t sz = 1 + (rng() % 16);
      char *ptr = pool.allocateAndDefaultConstruct(static_cast<aeronet::ObjectArrayPool<char>::size_type>(sz));
      allocated.emplace_back(ptr, sz);
      // write some sentinel bytes
      for (size_t i = 0; i < sz; ++i) {
        ptr[i] = static_cast<char>((rng() % 26) + 'a');
      }
      lastWasAlloc = true;
    } else if (choice == 1 && !allocated.empty()) {
      // shrink last allocated if any
      if (lastWasAlloc) {
        auto &last = allocated.back();
        size_t newSize = (rng() % (last.second + 1));
        pool.shrinkLastAllocated(last.first, static_cast<aeronet::ObjectArrayPool<char>::size_type>(newSize));
        last.second = newSize;
      }
      lastWasAlloc = false;
    } else if (choice == 2) {
      // clear
      pool.clear();
      allocated.clear();
      lastWasAlloc = false;
    } else if (choice == 3) {
      // reset
      pool.reset();
      allocated.clear();
      lastWasAlloc = false;
    }
  }
}

void RunFuzzPoolString(unsigned seed) {
  std::mt19937_64 rng(seed);
  ObjectArrayPool<std::string> pool(8);
  std::vector<std::pair<std::string *, size_t>> allocated;
  bool lastWasAlloc = false;

  for (int op = 0; op < 1000; ++op) {
    int choice = rng() % 4;
    if (choice == 0) {
      size_t sz = 1 + (rng() % 12);
      std::string *ptr =
          pool.allocateAndDefaultConstruct(static_cast<aeronet::ObjectArrayPool<std::string>::size_type>(sz));
      allocated.emplace_back(ptr, sz);
      for (size_t i = 0; i < sz; ++i) {
        ptr[i] = std::string("str") + std::to_string(rng() % 100);
      }
      lastWasAlloc = true;
    } else if (choice == 1 && !allocated.empty()) {
      if (lastWasAlloc) {
        auto &last = allocated.back();
        size_t newSize = (rng() % (last.second + 1));
        pool.shrinkLastAllocated(last.first, static_cast<aeronet::ObjectArrayPool<std::string>::size_type>(newSize));
        last.second = newSize;
        if (last.second == 0) {
          allocated.pop_back();
        }
      }
      lastWasAlloc = false;
    } else if (choice == 2) {
      pool.clear();
      allocated.clear();
      lastWasAlloc = false;
    } else if (choice == 3) {
      pool.reset();
      allocated.clear();
      lastWasAlloc = false;
    }
  }
}
}  // namespace

TEST(ObjectArrayPoolFuzzTest, CharFuzzDeterministic) { RunFuzzPoolChar(12345); }

TEST(ObjectArrayPoolFuzzTest, StringFuzzDeterministic) { RunFuzzPoolString(67890); }

}  // namespace aeronet