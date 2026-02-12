#pragma once

#include <cassert>
#include <cstring>
#include <string_view>

namespace aeronet {

constexpr void Copy(std::string_view sv, char* dst) noexcept {
  assert(!sv.empty());
  std::memcpy(dst, sv.data(), sv.size());
}

[[nodiscard]] constexpr char* Append(std::string_view sv, char* dst) noexcept {
  Copy(sv, dst);
  return dst + sv.size();
}

}  // namespace aeronet