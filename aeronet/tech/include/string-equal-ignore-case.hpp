#pragma once

#include <string_view>

#include "toupperlower.hpp"

namespace aeronet {

constexpr bool CaseInsensitiveEqual(std::string_view lhs, std::string_view rhs) {
  const auto lhsSize = lhs.size();
  if (lhsSize != rhs.size()) {
    return false;
  }
  for (std::string_view::size_type charPos{}; charPos < lhsSize; ++charPos) {
    if (toupper(lhs[charPos]) != toupper(rhs[charPos])) {
      return false;
    }
  }
  return true;
}

constexpr bool CaseInsensitiveLess(std::string_view lhs, std::string_view rhs) {
  const auto lhsSize = lhs.size();
  const auto rhsSize = rhs.size();
  for (std::string_view::size_type charPos = 0; charPos < lhsSize && charPos < rhsSize; ++charPos) {
    const auto lhsChar = toupper(lhs[charPos]);
    const auto rhsChar = toupper(rhs[charPos]);
    if (lhsChar != rhsChar) {
      return lhsChar < rhsChar;
    }
  }
  return lhsSize < rhsSize;
}

}  // namespace aeronet
