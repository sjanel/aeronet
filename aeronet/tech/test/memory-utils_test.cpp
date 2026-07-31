#include "aeronet/memory-utils.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include "aeronet/memory-utils-sv.hpp"

namespace aeronet {

namespace {

// Copy into a guarded buffer and assert the bytes landed exactly, with neither under- nor over-write.
// The overlapping fast path reads/writes [0, k) and [len - k, len); these checks pin down that it never
// touches a byte outside [dst, dst + len).
template <typename CopyFn>
void CheckGuardedCopy(std::string_view sv, CopyFn copyFn) {
  constexpr char kGuard = '\x7f';
  constexpr std::size_t kPad = 8;
  std::string buf(sv.size() + (2 * kPad), kGuard);
  char* dst = buf.data() + kPad;

  copyFn(dst);

  EXPECT_EQ(std::string_view(dst, sv.size()), sv) << "len=" << sv.size();
  // Leading guard bytes untouched.
  for (std::size_t i = 0; i < kPad; ++i) {
    EXPECT_EQ(buf[i], kGuard) << "underwrite at i=" << i << " len=" << sv.size();
  }
  // Trailing guard bytes untouched.
  for (std::size_t i = 0; i < kPad; ++i) {
    EXPECT_EQ(buf[kPad + sv.size() + i], kGuard) << "overwrite at i=" << i << " len=" << sv.size();
  }
}

void CheckCopy(std::string_view sv) {
  CheckGuardedCopy(sv, [sv](char* dst) { Copy(sv, dst); });
}

void CheckRawCopy(std::string_view sv) {
  CheckGuardedCopy(sv, [sv](char* dst) { Copy(sv.data(), sv.size(), dst); });
}

constexpr bool CheckConstexprRawCopy() {
  constexpr std::array<char, 6> source{'a', 'e', 'r', 'o', 'n', 'e'};
  std::array<char, source.size()> destination{};
  Copy(source.data(), source.size(), destination.data());
  return source == destination;
}

static_assert(CheckConstexprRawCopy());

}  // namespace

TEST(MemoryUtilsCopy, AllSizesAcrossDispatchBoundaries) {
  // Build a deterministic payload and copy every length, exercising each size class
  // (1, [2,3], [4,7], [8,15], [16,32]) and the >32 memcpy fallback, including the exact boundaries.
  // Copy requires len >= 1 (asserted precondition), so we start at 1.
  std::string payload;
  payload.reserve(48);
  for (std::size_t i = 0; i < 48; ++i) {
    payload.push_back(static_cast<char>('A' + (i % 26)));
  }
  for (std::size_t len = 1; len <= payload.size(); ++len) {
    CheckCopy(std::string_view(payload.data(), len));
  }
}

TEST(MemoryUtilsCopy, BoundaryLengths) {
  // Spot-check the off-by-one neighbours of every internal threshold.
  const std::string base(64, 'q');
  for (std::size_t len : {
           std::size_t{1},
           std::size_t{2},
           std::size_t{3},
           std::size_t{4},
           std::size_t{7},
           std::size_t{8},
           std::size_t{15},
           std::size_t{16},
           std::size_t{17},
           std::size_t{31},
           std::size_t{32},
           std::size_t{33},
           std::size_t{64},
       }) {
    CheckCopy(std::string_view(base.data(), len));
  }
}

TEST(MemoryUtilsRawCopy, MultipleSizeTypesAcrossDispatchBoundaries) {
  std::string payload;
  payload.reserve(48);
  for (std::size_t i = 0; i < 48; ++i) {
    payload.push_back(static_cast<char>('A' + (i % 26)));
  }

  for (std::size_t len = 0; len <= payload.size(); ++len) {
    const std::string_view sv(payload.data(), len);

    CheckRawCopy(sv);
  }
}

TEST(MemoryUtilsRawCopy, SupportsBytePointers) {
  constexpr std::byte source[]{
      std::byte{0x00}, std::byte{0x11}, std::byte{0x7f}, std::byte{0x80}, std::byte{0xff},
  };
  std::array<std::byte, std::size(source)> destination{};

  Copy(source, std::size(source), destination.data());

  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(destination.data()), std::size(destination)),
            std::string_view(reinterpret_cast<const char*>(source), std::size(source)));
}

TEST(MemoryUtilsAppend, ReturnsAdvancedPointerAndCopies) {
  std::array<char, 32> buf{};
  char* end = Append(std::string_view("Host: "), buf.data());
  EXPECT_EQ(end, buf.data() + 6);
  end = Append(std::string_view("x"), end);
  EXPECT_EQ(end, buf.data() + 7);
  EXPECT_EQ(std::string_view(buf.data(), 7), "Host: x");
}

}  // namespace aeronet
