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
  static constexpr char kHexits[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

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
  static constexpr char kHexits[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

  buf[0] = kHexits[ch >> 4U];
  buf[1] = kHexits[ch & 0x0F];

  return buf + 2;
}

constexpr char* to_upper_hex(char ch, char* buf) { return to_upper_hex(static_cast<unsigned char>(ch), buf); }

/// Decode a single hexadecimal digit. Returns -1 if invalid.
constexpr int8_t from_hex_digit(char ch) {
  struct HexTable {
    int8_t data[256];
  };

  static constexpr HexTable kHexTable = [] {
    HexTable table;
    for (signed char& val : table.data) {
      val = -1;
    }
    for (std::size_t i = 0; i < 10; ++i) {
      table.data['0' + i] = static_cast<int8_t>(i);
    }
    for (std::size_t i = 0; i < 6; ++i) {
      table.data['A' + i] = static_cast<int8_t>(10 + i);
      table.data['a' + i] = static_cast<int8_t>(10 + i);
    }
    return table;
  }();

  return kHexTable.data[static_cast<unsigned char>(ch)];
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
    *--out = kHexits[value & 0x0FU];
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