#pragma once

#include <cctype>
#include <cstddef>
#include <functional>
#include <string_view>

#include "toupperlower.hpp"

namespace aeronet {

constexpr bool CaseInsensitiveEqual(std::string_view lhs, std::string_view rhs) {
  const auto lhsSize = lhs.size();
  if (lhsSize != rhs.size()) {
    return false;
  }
  for (std::string_view::size_type charPos{}; charPos < lhsSize; ++charPos) {
    if (tolower(lhs[charPos]) != tolower(rhs[charPos])) {
      return false;
    }
  }
  return true;
}

constexpr bool CaseInsensitiveLess(std::string_view lhs, std::string_view rhs) {
  const auto lhsSize = lhs.size();
  const auto rhsSize = rhs.size();
  for (std::string_view::size_type charPos = 0; charPos < lhsSize && charPos < rhsSize; ++charPos) {
    const auto lhsChar = tolower(lhs[charPos]);
    const auto rhsChar = tolower(rhs[charPos]);
    if (lhsChar != rhsChar) {
      return lhsChar < rhsChar;
    }
  }
  return lhsSize < rhsSize;
}

struct CaseInsensitiveHashFunc {
  std::size_t operator()(std::string_view s) const noexcept {
    std::size_t h = 0;
    std::hash<char> hc;
    for (char ch : s) {
      h ^= hc(tolower(ch)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
  }
};

struct CaseInsensitiveEqualFunc {
  bool operator()(std::string_view lhs, std::string_view rhs) const noexcept { return CaseInsensitiveEqual(lhs, rhs); }
};

}  // namespace aeronet
