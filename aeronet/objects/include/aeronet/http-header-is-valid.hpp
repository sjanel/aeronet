#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/tchars.hpp"

namespace aeronet::http {

constexpr bool IsValidHeaderName(std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }
  struct AllowedTable {
    bool data[256];
  };
  static constexpr auto kNameAllowed = [] {
    AllowedTable allowed;
    for (std::size_t i = 0; i < sizeof(allowed.data); ++i) {
      allowed.data[static_cast<unsigned char>(i)] = is_tchar(static_cast<char>(i));
    }
    return allowed;
  }();

  // NOLINTNEXTLINE(readability-use-anyofallof)
  for (auto ch : name) {
    if (!kNameAllowed.data[static_cast<unsigned char>(ch)]) {
      return false;
    }
  }
  return true;
}

constexpr bool IsValidHeaderValue(std::string_view value) noexcept {
  struct AllowedTable {
    bool data[256];
  };
  static constexpr auto kValueAllowed = [] {
    AllowedTable allowed;
    for (std::size_t i = 0; i < sizeof(allowed.data); ++i) {
      const unsigned char ch = static_cast<unsigned char>(i);
      if (ch == '\r' || ch == '\n') {
        allowed.data[ch] = false;
      } else if (ch == '\t') {
        allowed.data[ch] = true;
      } else {
        allowed.data[ch] = ch >= 0x20 && ch <= 0x7E;
      }
    }
    return allowed;
  }();

  // NOLINTNEXTLINE(readability-use-anyofallof)
  for (auto ch : value) {
    if (!kValueAllowed.data[static_cast<unsigned char>(ch)]) {
      return false;
    }
  }
  return true;
}

}  // namespace aeronet::http