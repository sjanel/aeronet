#pragma once

#include <cassert>
#include <cstring>
#include <string_view>

namespace aeronet {

constexpr void Copy(std::string_view sv, char* dst) noexcept {
  // This asserts aim to ensure that we don't invoke std::memcpy undefined behavior.
  // See https://en.cppreference.com/w/cpp/string/byte/memcpy.html
  assert(dst != nullptr && sv.data() != nullptr);
  std::memcpy(dst, sv.data(), sv.size());
}

[[nodiscard]] constexpr char* Append(std::string_view sv, char* dst) noexcept {
  Copy(sv, dst);
  return dst + sv.size();
}

}  // namespace aeronet