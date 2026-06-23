#pragma once

#include <string_view>

#include "aeronet/memory-utils.hpp"

namespace aeronet {

// std::string_view conveniences over the raw-pointer Copy in memory-utils.hpp.
//
// These live in a separate header so that translation units copying through raw byte pointers only
// (the common hot-path case) can include memory-utils.hpp without dragging in <string_view>.

// Copy sv.size() bytes from sv into pDes, as of Copy(sv.data(), sv.size(), pDes).
constexpr void Copy(std::string_view sv, char* pDes) noexcept { Copy(sv.data(), sv.size(), pDes); }

// Copy sv into pDes and return the past-the-end pointer of the written region (pDes + sv.size()).
[[nodiscard]] constexpr char* Append(std::string_view sv, char* pDes) noexcept {
  Copy(sv, pDes);
  return pDes + sv.size();
}

}  // namespace aeronet
