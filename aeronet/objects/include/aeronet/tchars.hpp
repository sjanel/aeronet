#pragma once

#include <cstdint>

namespace aeronet {

/// RFC 7230: tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*"
///                  / "+" / "-" / "." / "^" / "_" / "`" / "|" / "~"
///                  / DIGIT / ALPHA
constexpr bool is_tchar(unsigned char uc) noexcept {
  // Two 64-bit chunks: [0–63], [64–127]
  static constexpr uint64_t bitmap[]{
      // low 64
      (1ULL << static_cast<uint8_t>('!')) | (1ULL << static_cast<uint8_t>('#')) | (1ULL << static_cast<uint8_t>('$')) |
          (1ULL << static_cast<uint8_t>('%')) | (1ULL << static_cast<uint8_t>('&')) |
          (1ULL << static_cast<uint8_t>('\'')) | (1ULL << static_cast<uint8_t>('*')) |
          (1ULL << static_cast<uint8_t>('+')) | (1ULL << static_cast<uint8_t>('-')) |
          (1ULL << static_cast<uint8_t>('.')) | (0x3FFULL << static_cast<uint8_t>('0')),  // digits 0–9

      // high 64 (offset by -64)
      (0x3FFFFFFULL << static_cast<uint8_t>('A' - 64)) |                                         // A–Z
          (1ULL << static_cast<uint8_t>('^' - 64)) | (1ULL << static_cast<uint8_t>('_' - 64)) |  // ^ and _
          (0x3FFFFFFULL << static_cast<uint8_t>('a' - 64)) |                                     // a–z
          (1ULL << static_cast<uint8_t>('`' - 64)) | (1ULL << static_cast<uint8_t>('|' - 64)) |
          (1ULL << static_cast<uint8_t>('~' - 64)),
  };

  return uc < 128U && ((bitmap[uc >> 6U] >> (uc & 63U)) & 1U) != 0U;
}

constexpr bool is_tchar(char ch) noexcept { return is_tchar(static_cast<unsigned char>(ch)); }

}  // namespace aeronet