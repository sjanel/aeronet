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
void CheckCopy(std::string_view sv) {
  constexpr char kGuard = '\x7f';
  constexpr std::size_t kPad = 8;
  std::string buf(sv.size() + (2 * kPad), kGuard);
  char* dst = buf.data() + kPad;

  Copy(sv, dst);

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
  for (std::size_t len :
       {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4}, std::size_t{7}, std::size_t{8}, std::size_t{15},
        std::size_t{16}, std::size_t{17}, std::size_t{31}, std::size_t{32}, std::size_t{33}, std::size_t{64}}) {
    CheckCopy(std::string_view(base.data(), len));
  }
}

TEST(MemoryUtilsAppend, ReturnsAdvancedPointerAndCopies) {
  std::array<char, 32> buf{};
  char* end = Append(std::string_view("Host: "), buf.data());
  EXPECT_EQ(end, buf.data() + 6);
  end = Append(std::string_view("x"), end);
  EXPECT_EQ(end, buf.data() + 7);
  EXPECT_EQ(std::string_view(buf.data(), 7), "Host: x");
}

TEST(MemoryUtilsSearchCRLF, FindsAndReportsAbsence) {
  // SearchCRLF operates on raw char pointers (it uses std::memchr internally), so feed it
  // data()/data()+size() rather than begin()/end(): on libstdc++/libc++ string_view::iterator
  // is const char*, but on MSVC it is a wrapper class that does not convert to const char*.
  std::string_view withCrlf = "abc\r\ndef";
  const char* withCrlfBeg = withCrlf.data();
  const char* withCrlfEnd = withCrlfBeg + withCrlf.size();
  const char* it = SearchCRLF(withCrlfBeg, withCrlfEnd);
  ASSERT_NE(it, withCrlfEnd);
  EXPECT_EQ(static_cast<std::size_t>(it - withCrlfBeg), 3U);

  // A lone CR (no following LF) must not be reported as a CRLF.
  std::string_view loneCr = "abc\rdef";
  const char* loneCrBeg = loneCr.data();
  const char* loneCrEnd = loneCrBeg + loneCr.size();
  EXPECT_EQ(SearchCRLF(loneCrBeg, loneCrEnd), loneCrEnd);

  std::string_view none = "no line break here";
  const char* noneBeg = none.data();
  const char* noneEnd = noneBeg + none.size();
  EXPECT_EQ(SearchCRLF(noneBeg, noneEnd), noneEnd);

  // CR as the final byte: there is no room for a following LF.
  std::string_view trailingCr = "abc\r";
  const char* trailingCrBeg = trailingCr.data();
  const char* trailingCrEnd = trailingCrBeg + trailingCr.size();
  EXPECT_EQ(SearchCRLF(trailingCrBeg, trailingCrEnd), trailingCrEnd);
}

}  // namespace aeronet
