#pragma once

namespace aeronet {

constexpr bool isdigit(char ch) { return ch >= '0' && ch <= '9'; }

constexpr bool islower(char ch) { return ch >= 'a' && ch <= 'z'; }

constexpr bool isspace(char ch) { return ch == ' ' || (ch >= '\t' && ch <= '\r'); }

// The ASCII whitespace of the WHATWG Infra standard (HTML, DOM, URL): isspace without the vertical tab.
constexpr bool IsAsciiWhitespace(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r';
}

}  // namespace aeronet