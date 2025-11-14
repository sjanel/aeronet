#pragma once

namespace aeronet {

constexpr unsigned char tolower(unsigned char ch) {
  if (ch >= 'A' && ch <= 'Z') {
    ch |= 0x20;
  }
  return ch;
}

constexpr char tolower(char ch) { return static_cast<char>(tolower(static_cast<unsigned char>(ch))); }

constexpr unsigned char toupper(unsigned char ch) {
  if (ch >= 'a' && ch <= 'z') {
    ch &= 0xDF;  // clear lowercase bit
  }
  return ch;
}

constexpr char toupper(char ch) { return static_cast<char>(toupper(static_cast<unsigned char>(ch))); }

}  // namespace aeronet
