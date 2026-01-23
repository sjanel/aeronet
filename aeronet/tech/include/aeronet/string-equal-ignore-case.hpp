#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

#include "aeronet/case-insensitive-city.hpp"
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
  static std::size_t operator()(std::string_view str) noexcept {
    return static_cast<std::size_t>(CityHash64CI(str.data(), str.size()));
  }
};

struct CaseInsensitiveEqualFunc {
  static constexpr bool operator()(std::string_view lhs, std::string_view rhs) noexcept {
    return CaseInsensitiveEqual(lhs, rhs);
  }
};

}  // namespace aeronet
