#pragma once

#include <cstddef>
#include <cstring>

#include "aeronet/compiler-config.hpp"

namespace aeronet {

// Copy sz bytes from pSrc into pDes, as of std::memcpy(pDest, pSrc, sz).
//
// For a compile-time-constant size (the common case: literal header fragments such as CRLF or "Host: ")
// the size-dispatch below folds away entirely and the compiler emits direct stores, exactly as a bare
// std::memcpy would. For sizes <= 32 we instead
// emit a couple of overlapping fixed-width stores inline, avoiding the call. Microbenchmarks
// (benchmarks/internal/memory-utils_bench.cpp) show ~1.9x on isolated small copies and ~1.5x on a realistic HTTP
// fragment mix, with no regression above the threshold (it falls straight back to std::memcpy).
// General-purpose copy for sizes only known at runtime (bounded small-size
// branch table to avoid a real memcpy() call for short lengths). For
// compile-time-constant sizes, prefer CopyFixed()/AppendFixed() instead —
// it produces smaller, faster code in that case.
constexpr void Copy(const auto* AERONET_RESTRICT pSrc, std::size_t sz, auto* AERONET_RESTRICT pDes) noexcept {
  static_assert(sizeof(*pSrc) == 1 && sizeof(*pDes) == 1, "Copy only works for byte pointers");
  if consteval {
    for (std::size_t idx = 0; idx < sz; ++idx) {
      pDes[idx] = pSrc[idx];
    }
    return;
  }
  if (sz > 32U) {
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
  else if (sz >= 16U) {
    std::memcpy(pDes, pSrc, 16U);
    std::memcpy(pDes + sz - 16U, pSrc + sz - 16U, 16U);
  } else if (sz >= 8U) {
    std::memcpy(pDes, pSrc, 8U);
    std::memcpy(pDes + sz - 8U, pSrc + sz - 8U, 8U);
  } else if (sz >= 4U) {
    std::memcpy(pDes, pSrc, 4U);
    std::memcpy(pDes + sz - 4U, pSrc + sz - 4U, 4U);
  } else if (sz >= 2U) {
    std::memcpy(pDes, pSrc, 2U);
    std::memcpy(pDes + sz - 2U, pSrc + sz - 2U, 2U);
  } else if (sz == 1U) {
    pDes[0] = pSrc[0];
  }
}

// Append sz bytes from pSrc into pDes, returning a pointer to the first byte after the appended data.
constexpr auto* Append(const auto* AERONET_RESTRICT pSrc, std::size_t sz, auto* AERONET_RESTRICT pDes) noexcept {
  Copy(pSrc, sz, pDes);
  return pDes + sz;
}

}  // namespace aeronet
