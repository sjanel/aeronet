#pragma once

#include <bit>
#include <concepts>
#include <cstdint>
#include <type_traits>

namespace aeronet {

/// Returns the number of hexadecimal digits required to represent 'value' (no leading zeros).
/// Note: HexDigits(0) == 1.
constexpr uint8_t HexDigits(std::unsigned_integral auto value) noexcept {
  if (value == 0) {
    return 1U;
  }
  return static_cast<uint8_t>((std::bit_width(value) + 3) / 4);
}

/// Writes lowercase hexadecimal representation of 'value' to 'buf' (no leading zeros).
/// Caller must ensure buf has at least HexDigits(value) bytes.
/// Letters will be in lower case.
/// Return a pointer to the char immediately positioned after the written hexadecimal code.
/// Examples:
///  ',' -> "2c"
///  '?' -> "3f"
///  "hello" -> "68656c6c6f"
constexpr char* ToLowerHex(std::integral auto value, char* buf) {
  static constexpr char kHexits[]{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

  if constexpr (std::is_same_v<decltype(value), char> || std::is_same_v<decltype(value), signed char> ||
                std::is_same_v<decltype(value), unsigned char>) {
    *buf++ = kHexits[static_cast<unsigned char>(value) >> 4U];
    *buf++ = kHexits[static_cast<unsigned char>(value) & 0x0FU];

    return buf;
  } else if constexpr (std::unsigned_integral<decltype(value)>) {
    char* end = buf + HexDigits(value);
    char* out = end;

    // Write backwards to avoid reversal.
    do {
      *--out = kHexits[value & 0x0FU];
      value >>= 4U;
    } while (value != 0);

    return end;
  } else {
    static_assert(false, "ToLowerHex only accepts char or unsigned integral types");
  }
}

constexpr char* ToUpperHex(std::integral auto value, char* buf) {
  static constexpr char kHexits[]{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

  if constexpr (std::is_same_v<decltype(value), char> || std::is_same_v<decltype(value), signed char> ||
                std::is_same_v<decltype(value), unsigned char>) {
    *buf++ = kHexits[static_cast<unsigned char>(value) >> 4U];
    *buf++ = kHexits[static_cast<unsigned char>(value) & 0x0FU];

    return buf;
  } else {
    static_assert(false, "ToUpperHex only accepts char or unsigned char types");
  }
}

/// Decode a single hexadecimal digit. Returns -1 if invalid.
constexpr int8_t FromHexDigit(char ch) {
  struct HexTable {
    int8_t data[256];
  };

  static constexpr HexTable kHexTable = [] {
    HexTable table;
    for (auto& val : table.data) {
      val = -1;
    }
    for (int i = 0; i < 10; ++i) {
      table.data['0' + i] = static_cast<int8_t>(i);
    }
    for (int i = 0; i < 6; ++i) {
      table.data['A' + i] = static_cast<int8_t>(10 + i);
      table.data['a' + i] = static_cast<int8_t>(10 + i);
    }
    return table;
  }();

  return kHexTable.data[static_cast<unsigned char>(ch)];
}

}  // namespace aeronet