#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace aeronet {

// Base64url (RFC 4648 §5) without padding — the encoding mandated by JOSE/JWT (RFC 7515 §2,
// RFC 7519). Differs from standard base64 in two ways: the alphabet uses '-' and '_' instead of
// '+' and '/', and the trailing '=' padding is omitted. Decoding tolerates (but does not require)
// padding to stay liberal with third-party tokens.

namespace detail {
inline constexpr char kB64UrlTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// 6-bit value of a base64url symbol, or -1 when the character is not in the alphabet. Computed
// inline (every branch runs during decoding) rather than via a compile-time lookup table.
[[nodiscard]] constexpr int B64UrlSextet(char ch) noexcept {
  if (ch >= 'A' && ch <= 'Z') {
    return ch - 'A';
  }
  if (ch >= 'a' && ch <= 'z') {
    return ch - 'a' + 26;
  }
  if (ch >= '0' && ch <= '9') {
    return ch - '0' + 52;
  }
  if (ch == '-') {
    return 62;
  }
  if (ch == '_') {
    return 63;
  }
  return -1;
}
}  // namespace detail

// Number of characters produced when base64url-encoding `binDataLen` bytes (no padding).
[[nodiscard]] constexpr std::size_t B64UrlEncodedLen(std::size_t binDataLen) noexcept {
  return ((binDataLen / 3) * 4) + (binDataLen % 3 == 0 ? 0 : (binDataLen % 3) + 1);
}

// Encode `binData` into `out`, which must have room for B64UrlEncodedLen(binData.size()) chars.
constexpr void B64UrlEncode(std::span<const char> binData, char* out) noexcept {
  int bitsCollected{};
  uint32_t accumulator{};
  static constexpr auto kNbBits = 6;
  static constexpr uint32_t kMask6 = (1U << kNbBits) - 1U;
  for (char ch : binData) {
    accumulator = (accumulator << 8U) | static_cast<uint8_t>(ch);
    bitsCollected += 8;
    while (bitsCollected >= kNbBits) {
      bitsCollected -= kNbBits;
      *out++ = detail::kB64UrlTable[(accumulator >> static_cast<uint32_t>(bitsCollected)) & kMask6];
    }
  }
  if (bitsCollected > 0) {
    accumulator <<= static_cast<uint32_t>(kNbBits - bitsCollected);
    *out++ = detail::kB64UrlTable[accumulator & kMask6];
  }
}

// Upper bound on the number of bytes produced by decoding `encodedLen` base64url characters.
[[nodiscard]] constexpr std::size_t B64UrlMaxDecodedLen(std::size_t encodedLen) noexcept {
  return ((encodedLen / 4) * 3) + 2;
}

// Decode base64url `in` into `out` (sized via B64UrlMaxDecodedLen). On success writes the byte
// count to `outLen` and returns true. Returns false on any non-alphabet character or an invalid
// length (a lone trailing 6-bit group cannot exist). Optional '=' padding is accepted and ignored.
[[nodiscard]] constexpr bool B64UrlDecode(std::string_view in, char* out, std::size_t& outLen) noexcept {
  // Trim optional padding so the remaining length drives the bit accounting.
  while (!in.empty() && in.back() == '=') {
    in.remove_suffix(1);
  }
  if (in.size() % 4 == 1) {
    return false;  // a single leftover character carries only 6 bits — never valid
  }
  int bitsCollected{};
  uint32_t accumulator{};
  char* outStart = out;
  for (char ch : in) {
    const int val = detail::B64UrlSextet(ch);
    if (val < 0) {
      return false;
    }
    accumulator = (accumulator << 6U) | static_cast<uint32_t>(val);
    bitsCollected += 6;
    if (bitsCollected >= 8) {
      bitsCollected -= 8;
      *out++ = static_cast<char>((accumulator >> static_cast<uint32_t>(bitsCollected)) & 0xFFU);
    }
  }
  outLen = static_cast<std::size_t>(out - outStart);
  return true;
}

}  // namespace aeronet
