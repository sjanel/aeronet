#include "aeronet/secure-random.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#ifdef AERONET_LINUX
#include <cerrno>
#include <system_error>

#define AERONET_WANT_GETRANDOM_OVERRIDES
#include "aeronet/sys-test-support.hpp"
#endif

namespace aeronet {

TEST(SecureRandomTest, FillsTheWholeBuffer) {
  // 1 KiB: larger than what getrandom() always returns whole (256 bytes).
  std::array<std::byte, 1024> first{};
  std::array<std::byte, 1024> second{};
  FillSecureRandomBytes(first);
  FillSecureRandomBytes(second);
  EXPECT_NE(first, second);
  // Each 64-byte block was written: the odds of 64 zero bytes in a row are nil.
  for (std::size_t pos = 0; pos < first.size(); pos += 64) {
    const auto block = std::span(first).subspan(pos, 64);
    EXPECT_FALSE(std::ranges::all_of(block, [](std::byte byte) { return byte == std::byte{0}; })) << pos;
  }
  FillSecureRandomBytes({});
}

#ifdef AERONET_LINUX
TEST(SecureRandomTest, RetriesInterruptedAndShortReads) {
  test::g_getrandom_actions.setActions({{-1, EINTR}, {3, 0}, {-1, EINTR}});
  std::array<std::byte, 64> bytes{};
  FillSecureRandomBytes(bytes);
  EXPECT_TRUE(test::g_getrandom_actions.empty());
  EXPECT_FALSE(std::ranges::all_of(std::span(bytes).subspan(3), [](std::byte byte) { return byte == std::byte{0}; }));
}

TEST(SecureRandomTest, ThrowsOnFailure) {
  test::g_getrandom_actions.setActions({{-1, ENOSYS}});
  std::array<std::byte, 16> bytes{};
  EXPECT_THROW(FillSecureRandomBytes(bytes), std::system_error);
  test::g_getrandom_actions.reset();
}
#endif

}  // namespace aeronet
