#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

#include "aeronet/toupperlower.hpp"

namespace aeronet {

constexpr bool CaseInsensitiveEqual(std::string_view lhs, std::string_view rhs) {
  const auto lhsSize = lhs.size();
  if (lhsSize != rhs.size()) {
    return false;
  }

  const char* pLhs = lhs.data();
  const char* pRhs = rhs.data();
  const char* end = pLhs + lhsSize;

  for (; pLhs != end; ++pLhs, ++pRhs) {
    if (tolower(*pLhs) != tolower(*pRhs)) {
      return false;
    }
  }
  return true;
}

constexpr bool CaseInsensitiveLess(std::string_view lhs, std::string_view rhs) {
  const auto lhsSize = lhs.size();
  const auto rhsSize = rhs.size();
  const auto minSize = lhsSize < rhsSize ? lhsSize : rhsSize;

  const char* pLhs = lhs.data();
  const char* pRhs = rhs.data();

  for (std::size_t i = 0; i < minSize; ++i) {
    const auto lc = tolower(pLhs[i]);
    const auto rc = tolower(pRhs[i]);
    if (lc != rc) {
      return lc < rc;
    }
  }
  return lhsSize < rhsSize;
}

struct CaseInsensitiveHashFunc {
  static constexpr std::size_t operator()(std::string_view str) noexcept {
    // FNV-1a hash, case insensitive
    std::size_t hash = 14695981039346656037ULL;
    for (char ch : str) {
      hash ^= tolower(static_cast<unsigned char>(ch));
      hash *= 1099511628211ULL;
    }
    return hash;
  }
};

struct CaseInsensitiveEqualFunc {
  static constexpr bool operator()(std::string_view lhs, std::string_view rhs) noexcept {
    return CaseInsensitiveEqual(lhs, rhs);
  }
};

}  // namespace aeronet
