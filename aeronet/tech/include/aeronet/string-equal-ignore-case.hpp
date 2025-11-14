#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

#include "aeronet/toupperlower.hpp"

namespace aeronet {

constexpr bool CaseInsensitiveEqual(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  const char* pLhs = lhs.data();
  const char* pRhs = rhs.data();
  const char* end = pLhs + lhs.size();

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
    auto lc = tolower(pLhs[i]);
    auto rc = tolower(pRhs[i]);
    if (lc != rc) {
      return lc < rc;
    }
  }
  return lhsSize < rhsSize;
}

constexpr bool StartsWithCaseInsensitive(std::string_view value, std::string_view prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }

  const char* pVal = value.data();
  const char* pPre = prefix.data();
  const char* end = pPre + prefix.size();

  for (; pPre != end; ++pVal, ++pPre) {
    if (tolower(*pVal) != tolower(*pPre)) {
      return false;
    }
  }
  return true;
}

struct CaseInsensitiveHashFunc {
  constexpr std::size_t operator()(std::string_view str) const noexcept {
    std::size_t hash = 0;
    const char* beg = str.data();
    const char* end = beg + str.size();
    for (; beg != end; ++beg) {
      hash ^= static_cast<std::size_t>(tolower(*beg)) + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (hash << 6) +
              (hash >> 2);
    }
    return hash;
  }
};

struct CaseInsensitiveEqualFunc {
  constexpr bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
    return CaseInsensitiveEqual(lhs, rhs);
  }
};

}  // namespace aeronet
