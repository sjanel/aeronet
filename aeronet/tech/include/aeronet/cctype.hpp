#pragma once

namespace aeronet {

constexpr bool isdigit(char ch) { return ch >= '0' && ch <= '9'; }

constexpr bool islower(char ch) { return ch >= 'a' && ch <= 'z'; }

constexpr bool isspace(char ch) { return ch == ' ' || (ch >= '\t' && ch <= '\r'); }

}  // namespace aeronet