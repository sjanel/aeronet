#pragma once

#include <cstdint>

namespace aeronet {

/// RFC 7230: tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*"
///                  / "+" / "-" / "." / "^" / "_" / "`" / "|" / "~"
///                  / DIGIT / ALPHA
constexpr bool is_tchar(unsigned char uc) noexcept {
  // Two 64-bit chunks: [0–63], [64–127]
  static constexpr uint64_t bitmap[2] = {
      // low 64
      (1ULL << '!') | (1ULL << '#') | (1ULL << '$') | (1ULL << '%') | (1ULL << '&') | (1ULL << '\'') | (1ULL << '*') |
          (1ULL << '+') | (1ULL << '-') | (1ULL << '.') | (0x3FFULL << '0'),  // digits 0–9

      // high 64 (offset by -64)
      (0x3FFFFFFULL << ('A' - 64)) |                     // A–Z
          (1ULL << ('^' - 64)) | (1ULL << ('_' - 64)) |  // ^ and _
          (0x3FFFFFFULL << ('a' - 64)) |                 // a–z
          (1ULL << ('`' - 64)) | (1ULL << ('|' - 64)) | (1ULL << ('~' - 64))};

  return uc < 128U && ((bitmap[uc >> 6] >> (uc & 63)) & 1U) != 0U;
}

constexpr bool is_tchar(char ch) noexcept { return is_tchar(static_cast<unsigned char>(ch)); }

}  // namespace aeronet