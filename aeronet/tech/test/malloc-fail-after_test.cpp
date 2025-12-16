#include <gtest/gtest.h>

#include <cstdlib>

#include "aeronet/sys-test-support.hpp"

using namespace aeronet;

#if AERONET_WANT_MALLOC_OVERRIDES

TEST(MallocFailAfter, SkipOneThenFailNext) {
  // Ensure we can skip one successful allocation and have the second fail.
  test::FailNextMalloc(1, 1);  // allow 1 success, then 1 failure

  void* p1 = std::malloc(32);
  EXPECT_NE(p1, nullptr);
  std::free(p1);

  void* p2 = std::malloc(64);
  // The second allocation should fail (return nullptr) and set errno to ENOMEM
  EXPECT_EQ(p2, nullptr);

  void* p3 = std::malloc(16);
  // Subsequent allocations should succeed again
  EXPECT_NE(p3, nullptr);
  std::free(p3);
}

#endif