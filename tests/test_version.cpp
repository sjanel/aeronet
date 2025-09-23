#include <gtest/gtest.h>

#include "aeronet/version.hpp"

TEST(AeronetVersion, Version) {
  static constexpr auto kVersion = aeronet::version();
  EXPECT_FALSE(kVersion.empty());
}