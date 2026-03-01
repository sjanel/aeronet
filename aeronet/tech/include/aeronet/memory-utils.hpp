#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace aeronet {

inline void Copy(std::string_view sv, char* dst) noexcept {
  // This asserts aim to ensure that we don't invoke std::memcpy undefined behavior.
  // See https://en.cppreference.com/w/cpp/string/byte/memcpy.html
  assert(dst != nullptr && sv.data() != nullptr);
  std::memcpy(dst, sv.data(), sv.size());
}

[[nodiscard]] inline char* Append(std::string_view sv, char* dst) noexcept {
  Copy(sv, dst);
  return dst + sv.size();
}

// Search for CRLF in the range [begin, end). If found, return a pointer to the CR character. Otherwise, return end.
[[nodiscard]] inline auto SearchCRLF(auto begin, auto end) noexcept {
  while (begin != end) {
    begin = static_cast<decltype(begin)>(std::memchr(begin, '\r', static_cast<std::size_t>(end - begin)));
    if (begin == nullptr) {
      return end;
    }
    if (begin + 1 < end && *(begin + 1) == '\n') {
      return begin;
    }
    ++begin;
  }
  return end;
}

}  // namespace aeronet