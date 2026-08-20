#pragma once

#include <cstddef>
#include <cstring>

#include "aeronet/compiler-config.hpp"

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>

#include <bit>
#define AERONET_HAS_SEARCH_CRLF_SSE2 1
#endif

namespace aeronet {

namespace detail {

AERONET_ALWAYS_INLINE auto* SearchCRLFMemchr(auto* first, auto* last) noexcept {
  for (;; ++first) {
    first = static_cast<decltype(first)>(std::memchr(first, '\r', static_cast<std::size_t>(last - first)));
    if (first == nullptr) {
      return last;
    }
    if (first + 1 < last && first[1U] == '\n') {
      return first;
    }
  }
}

#ifdef AERONET_HAS_SEARCH_CRLF_SSE2
AERONET_ALWAYS_INLINE auto* SearchCRLFSse2Prefix(auto* first, auto* last) noexcept {
  // Observed scripted HTTP/1 benchmark traffic terminates every line inside this prefix (maximum 124 bytes).
  // Keep libc's vectorized memchr for the uncommon remainder. See benchmarks/internal/search-crlf_bench.cpp.
  static constexpr std::ptrdiff_t kPrefixBytes = 128;
  static_assert(kPrefixBytes != 0 && kPrefixBytes % 16 == 0);
  auto* const sseEnd = last - first < kPrefixBytes ? last : first + kPrefixBytes;
  if (sseEnd - first >= 16) {
    const __m128i cr = _mm_set1_epi8('\r');
    do {
      const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(first));
      auto mask = static_cast<unsigned int>(_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, cr)));
      while (mask != 0U) {
        const auto offset = std::countr_zero(mask);
        auto* const position = first + offset;
        if (position + 1 < last && position[1] == '\n') {
          return position;
        }
        mask &= mask - 1U;
      }
      first += 16;
    } while (sseEnd - first >= 16);
  }

  return SearchCRLFMemchr(first, last);
}
#endif

}  // namespace detail

// Search for CRLF in the range [begin, end). If found, return a pointer to the CR character. Otherwise, return end.
[[nodiscard]] AERONET_ALWAYS_INLINE auto* SearchCRLF(auto* first, auto* last) noexcept {
  static_assert(sizeof(*first) == 1, "SearchCRLF only works on byte ranges");
#ifdef AERONET_HAS_SEARCH_CRLF_SSE2
  return detail::SearchCRLFSse2Prefix(first, last);
#else
  return detail::SearchCRLFMemchr(first, last);
#endif
}

#ifdef AERONET_HAS_SEARCH_CRLF_SSE2
#undef AERONET_HAS_SEARCH_CRLF_SSE2
#endif

}  // namespace aeronet
