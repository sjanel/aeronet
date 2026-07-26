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

// Use with compile time string_views, for instance static constexpr string_view.
// Unlike Copy/Append, there is no branch table for small runtime sizes here - the whole point is to get out of the
// compiler's way and let its own memcpy-of-constant-size lowering pick the optimal instruction sequence (verified:
// identical codegen to a hand-written memcpy call, zero call overhead, no library memcpy() call in the binary).
template <const std::string_view& Sv>
inline void CopyFixed(auto* AERONET_RESTRICT pDes) noexcept {
  std::memcpy(pDes, Sv.data(), Sv.size());
}

// Copy sv into pDes and return the past-the-end pointer of the written region (pDes + sv.size()).
[[nodiscard]] constexpr char* Append(std::string_view sv, char* pDes) noexcept {
  Copy(sv, pDes);
  return pDes + sv.size();
}

template <const std::string_view& Sv>
[[nodiscard]] inline auto* AppendFixed(auto* AERONET_RESTRICT pDes) noexcept {
  CopyFixed<Sv>(pDes);
  return pDes + Sv.size();
}

}  // namespace aeronet
