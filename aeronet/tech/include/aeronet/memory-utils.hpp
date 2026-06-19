#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace aeronet {

// Copy sv.size() bytes from sv into dst.
//
// For a compile-time-constant size (the common case: literal header fragments such as CRLF or "Host: ")
// the size-dispatch below folds away entirely and the compiler emits direct stores, exactly as a bare
// std::memcpy would. The interesting case is a runtime-sized copy: a bare std::memcpy then lowers to a
// `call memcpy@PLT`, whose call/dispatch overhead dominates for the short fragments aeronet appends in bulk
// (method, header names/values). For sizes <= 32 we instead emit a couple of overlapping fixed-width stores
// inline, avoiding the call. Microbenchmarks (benchmarks/internal/memory-utils_bench.cpp) show ~1.9x on
// isolated small copies and ~1.5x on a realistic HTTP fragment mix, with no regression above the threshold
// (it falls straight back to std::memcpy).
//
// Safety: every access stays within [ptr, ptr + len) for both src and dst, so no source padding or
// destination slack is required and any caller of the old memcpy-based Copy remains correct.
//
// Precondition (unchanged from the original memcpy-based Copy): src and dst are non-null. aeronet never
// copies an empty/null view, and the assert documents and guards that. See
// https://en.cppreference.com/w/cpp/string/byte/memcpy.html for why std::memcpy on a null pointer is UB even
// for a zero-length copy. The size dispatch below performs no store for len == 0, so even a degenerate
// release-build call cannot corrupt memory.
inline void Copy(std::string_view sv, char* dst) noexcept {
  const char* src = sv.data();
  const std::size_t len = sv.size();
  assert(dst != nullptr && src != nullptr);
  if (len > 32) {
    std::memcpy(dst, src, len);
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
  else if (len >= 16) {
    std::memcpy(dst, src, 16);
    std::memcpy(dst + len - 16, src + len - 16, 16);
  } else if (len >= 8) {
    std::memcpy(dst, src, 8);
    std::memcpy(dst + len - 8, src + len - 8, 8);
  } else if (len >= 4) {
    std::memcpy(dst, src, 4);
    std::memcpy(dst + len - 4, src + len - 4, 4);
  } else if (len >= 2) {
    std::memcpy(dst, src, 2);
    std::memcpy(dst + len - 2, src + len - 2, 2);
  } else if (len == 1) {
    dst[0] = src[0];
  }
}

[[nodiscard]] inline char* Append(std::string_view sv, char* dst) noexcept {
  Copy(sv, dst);
  return dst + sv.size();
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