#pragma once

#include <array>
#include <string_view>

#include "aeronet/tchars.hpp"

namespace aeronet::http {

constexpr bool IsValidHeaderName(std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }
  static constexpr auto kNameAllowed = [] {
    std::array<bool, 256> allowed{};
    for (int i = 0; i < 256; ++i) {
      allowed[static_cast<unsigned char>(i)] = is_tchar(static_cast<char>(i));
    }
    return allowed;
  }();

  // NOLINTNEXTLINE(readability-use-anyofallof)
  for (auto ch : name) {
    if (!kNameAllowed[static_cast<unsigned char>(ch)]) {
      return false;
    }
  }
  return true;
}

constexpr bool IsValidHeaderValue(std::string_view value) noexcept {
  static constexpr auto kValueAllowed = [] {
    std::array<bool, 256> allowed{};
    for (int i = 0; i < 256; ++i) {
      const unsigned char ch = static_cast<unsigned char>(i);
      if (ch == '\r' || ch == '\n') {
        allowed[ch] = false;
      } else if (ch == '\t') {
        allowed[ch] = true;
      } else {
        allowed[ch] = ch >= 0x20 && ch <= 0x7E;
      }
    }
    return allowed;
  }();

  // NOLINTNEXTLINE(readability-use-anyofallof)
  for (auto ch : value) {
    if (!kValueAllowed[static_cast<unsigned char>(ch)]) {
      return false;
    }
  }
  return true;
}

}  // namespace aeronet::http