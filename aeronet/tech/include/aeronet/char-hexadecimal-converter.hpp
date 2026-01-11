#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace aeronet {

/// Writes to 'buf' the 2-char hexadecimal code of given char 'ch'.
/// Given buffer should have space for at least two chars.
/// Letters will be in lower case.
/// Return a pointer to the char immediately positioned after the written hexadecimal code.
/// Examples:
///  ',' -> "2c"
///  '?' -> "3f"
constexpr char* to_lower_hex(unsigned char ch, char* buf) {
  static constexpr const char* const kHexits = "0123456789abcdef";

  buf[0] = kHexits[ch >> 4U];
  buf[1] = kHexits[ch & 0x0F];

  return buf + 2;
}

constexpr char* to_lower_hex(char ch, char* buf) { return to_lower_hex(static_cast<unsigned char>(ch), buf); }

/// Writes to 'buf' the 2-char hexadecimal code of given char 'ch'.
/// Given buffer should have space for at least two chars.
/// Letters will be in upper case.
/// Return a pointer to the char immediately positioned after the written hexadecimal code.
/// Examples:
///  ',' -> "2C"
///  '?' -> "3F"
constexpr char* to_upper_hex(unsigned char ch, char* buf) {
  static constexpr const char* const kHexits = "0123456789ABCDEF";

  buf[0] = kHexits[ch >> 4U];
  buf[1] = kHexits[ch & 0x0F];

  return buf + 2;
}

constexpr char* to_upper_hex(char ch, char* buf) { return to_upper_hex(static_cast<unsigned char>(ch), buf); }

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

// -----------------------------------------------------------------------------
// size_t hexadecimal helpers (no allocation, performance-oriented)
// -----------------------------------------------------------------------------

/// Maximum number of hexadecimal digits needed to represent a size_t.
inline constexpr uint32_t kMaxHexDigitsSizeT = 2UL * sizeof(std::size_t);

/// Returns the number of hexadecimal digits required to represent 'value' (no leading zeros).
/// Note: hex_digits(0) == 1.
constexpr uint32_t hex_digits(std::size_t value) noexcept {
  if (value == 0) {
    return 1U;
  }
  return (static_cast<uint32_t>(std::bit_width(value)) + 3U) / 4U;
}

/// Writes lowercase hexadecimal representation of 'value' to 'buf' (no leading zeros).
/// Caller must ensure buf has at least kMaxHexDigitsSizeT bytes.
/// Returns pointer to the char immediately after the last written hex digit.
constexpr char* to_lower_hex(std::size_t value, char* buf) noexcept {
  static constexpr const char* const kHexits = "0123456789abcdef";

  const std::size_t digits = hex_digits(value);
  char* end = buf + digits;
  char* out = end;

  // Write backwards to avoid reversal.
  do {
    --out;
    *out = kHexits[value & 0x0FU];
    value >>= 4U;
  } while (value != 0);

  return end;
}

/// Writes uppercase hexadecimal representation of 'value' to 'buf' (no leading zeros).
/// Caller must ensure buf has at least kMaxHexDigitsSizeT bytes.
/// Returns pointer to the char immediately after the last written hex digit.
constexpr char* to_upper_hex(std::size_t value, char* buf) noexcept {
  static constexpr const char* const kHexits = "0123456789ABCDEF";

  const std::size_t digits = hex_digits(value);
  char* end = buf + digits;
  char* out = end;

  do {
    --out;
    *out = kHexits[value & 0x0FU];
    value >>= 4U;
  } while (value != 0);

  return end;
}

}  // namespace aeronet