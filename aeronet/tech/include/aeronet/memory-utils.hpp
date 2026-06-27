#pragma once

#include <cstddef>
#include <cstring>

namespace aeronet {

// Copy sz bytes from pSrc into pDes, as of std::memcpy(pDest, pSrc, sz).
//
// For a compile-time-constant size (the common case: literal header fragments such as CRLF or "Host: ")
// the size-dispatch below folds away entirely and the compiler emits direct stores, exactly as a bare
// std::memcpy would. For sizes <= 32 we instead
// emit a couple of overlapping fixed-width stores inline, avoiding the call. Microbenchmarks
// (benchmarks/internal/memory-utils_bench.cpp) show ~1.9x on isolated small copies and ~1.5x on a realistic HTTP
// fragment mix, with no regression above the threshold (it falls straight back to std::memcpy).
constexpr void Copy(const auto* pSrc, std::size_t sz, auto* pDes) noexcept {
  static_assert(sizeof(*pSrc) == 1 && sizeof(*pDes) == 1, "Copy only works for byte pointers");
  if consteval {
    for (std::size_t i = 0; i < sz; ++i) {
      pDes[i] = pSrc[i];
    }
    return;
  }
  if (sz > 32) {
    std::memcpy(pDes, pSrc, sz);
  }
  // Overlapping copies. Each branch picks the largest fixed chunk k in {16, 8, 4, 2, 1} with k <= len. Since
  // the next-larger branch was not taken, len < 2k, so the two k-byte windows [0, k) and [len - k, len) are
  // guaranteed to meet or overlap and together tile exactly [0, len) -- never a byte beyond it. The middle
  // bytes covered by both windows are simply written twice with identical data (harmless). Every chunk size
  // is a compile-time constant, so each std::memcpy lowers to a single (possibly vector) load/store, with no
  // call and no per-byte loop.
  //
  // Example for len = 10: 8 <= 10 < 16, so the k = 8 branch runs (NOT the 16-byte one):
  //
  //     position:        0  1  2  3  4  5  6  7  8  9          (only these 10 bytes are valid)
  //     memcpy(dst,   src,   8):  [ 0  1  2  3  4  5  6  7 ]
  //     memcpy(dst+2, src+2, 8):        [ 2  3  4  5  6  7  8  9 ]
  //                                      \________________/
  //                                      overlap [2, 8) rewritten with the same bytes
  //     union of writes = [0, 10): exact, no out-of-bounds store (16 bytes moved, 6 of them twice).
  else if (sz >= 16) {
    std::memcpy(pDes, pSrc, 16);
    std::memcpy(pDes + sz - 16, pSrc + sz - 16, 16);
  } else if (sz >= 8) {
    std::memcpy(pDes, pSrc, 8);
    std::memcpy(pDes + sz - 8, pSrc + sz - 8, 8);
  } else if (sz >= 4) {
    std::memcpy(pDes, pSrc, 4);
    std::memcpy(pDes + sz - 4, pSrc + sz - 4, 4);
  } else if (sz >= 2) {
    std::memcpy(pDes, pSrc, 2);
    std::memcpy(pDes + sz - 2, pSrc + sz - 2, 2);
  } else if (sz == 1) {
    pDes[0] = pSrc[0];
  }
}

// Append sz bytes from pSrc into pDes, returning a pointer to the first byte after the appended data.
constexpr auto* Append(const auto* pSrc, std::size_t sz, auto* pDes) noexcept {
  Copy(pSrc, sz, pDes);
  return pDes + sz;
}

// Search for CRLF in the range [begin, end). If found, return a pointer to the CR character. Otherwise, return end.
[[nodiscard]] inline auto* SearchCRLF(auto* begin, auto* end) noexcept {
  static_assert(sizeof(*begin) == 1, "SearchCRLF only works on byte ranges");
  for (; begin != end; ++begin) {
    begin = static_cast<decltype(begin)>(std::memchr(begin, '\r', static_cast<std::size_t>(end - begin)));
    if (begin == nullptr) {
      return end;
    }
    if (begin + 1 < end && begin[1U] == '\n') {
      return begin;
    }
  }
  return end;
}

}  // namespace aeronet