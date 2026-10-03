#pragma once

#include <cstddef>
#include <cstring>

namespace aeronet {

// std::find for byte ranges, backed by the vectorized libc memchr: returns a pointer to the first occurrence of ch in
// [first, last), or last if there is none.
// Prefer it over std::find unless the range is known to be only a few bytes long, where the call overhead of memchr
// makes a simple loop faster (see benchmarks/internal/init-try-set-head_bench.cpp).
template <class CharT>
[[nodiscard]] CharT* FindChar(CharT* first, CharT* last, char ch) noexcept {
  static_assert(sizeof(CharT) == 1, "FindChar only works on byte ranges");
  auto* pos = static_cast<CharT*>(std::memchr(first, ch, static_cast<std::size_t>(last - first)));
  return pos == nullptr ? last : pos;
}

}  // namespace aeronet
