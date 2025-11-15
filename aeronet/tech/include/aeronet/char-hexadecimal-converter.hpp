#pragma once

namespace aeronet {

/// Writes to 'buf' the 2-char hexadecimal code of given char 'ch'.
/// Given buffer should have space for at least two chars.
/// Letters will be in lower case.
/// Return a pointer to the char immediately positioned after the written hexadecimal code.
/// Examples:
///  ',' -> "2c"
///  '?' -> "3f"
constexpr char *to_lower_hex(unsigned char ch, char *buf) {
  static constexpr const char *const kHexits = "0123456789abcdef";

  buf[0] = kHexits[ch >> 4U];
  buf[1] = kHexits[ch & 0x0F];

  return buf + 2;
}

constexpr char *to_lower_hex(char ch, char *buf) { return to_lower_hex(static_cast<unsigned char>(ch), buf); }

/// Writes to 'buf' the 2-char hexadecimal code of given char 'ch'.
/// Given buffer should have space for at least two chars.
/// Letters will be in upper case.
/// Return a pointer to the char immediately positioned after the written hexadecimal code.
/// Examples:
///  ',' -> "2C"
///  '?' -> "3F"
constexpr char *to_upper_hex(unsigned char ch, char *buf) {
  static constexpr const char *const kHexits = "0123456789ABCDEF";

  buf[0] = kHexits[ch >> 4U];
  buf[1] = kHexits[ch & 0x0F];

  return buf + 2;
}

constexpr char *to_upper_hex(char ch, char *buf) { return to_upper_hex(static_cast<unsigned char>(ch), buf); }

/// Decode a single hexadecimal digit. Returns -1 if invalid.
constexpr int from_hex_digit(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'A' && ch <= 'F') {
    return 10 + (ch - 'A');
  }
  if (ch >= 'a' && ch <= 'f') {
    return 10 + (ch - 'a');
  }
  return -1;
}

}  // namespace aeronet