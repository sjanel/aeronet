#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace aeronet {

using B64Table = std::array<char, 64>;

// Build one of the two 64-symbol base64 alphabets from the 62 characters shared between
// standard base64 (RFC 4648 §4) and base64url (§5), plus the two symbols where they diverge.
consteval B64Table MakeB64Table(char sym62, char sym63) noexcept {
  B64Table table;
  std::size_t idx = 0;
  for (char ch = 'A'; ch <= 'Z'; ++ch) {
    table[idx++] = ch;
  }
  for (char ch = 'a'; ch <= 'z'; ++ch) {
    table[idx++] = ch;
  }
  for (char ch = '0'; ch <= '9'; ++ch) {
    table[idx++] = ch;
  }
  table[62] = sym62;
  table[63] = sym63;
  return table;
}

// The standard base64 alphabet (RFC 4648 §4), without the '=' padding character.
inline constexpr B64Table kB64Table = MakeB64Table('+', '/');

constexpr auto B64EncodedLen(auto binDataLen) noexcept { return static_cast<std::size_t>((binDataLen + 2U) / 3U) * 4U; }

// Encode `binData` into `pOut`, which must have room for B64EncodedLen(binData.size()) chars.
// Pads with '=' until 'pEndOut'.
void B64Encode(std::string_view binData, char* pOut, const char* pEndOut);

// Base64url (RFC 4648 §5) without padding — the encoding mandated by JOSE/JWT (RFC 7515 §2,
// RFC 7519). Differs from standard base64 in two ways: the alphabet uses '-' and '_' instead of
// '+' and '/', and the trailing '=' padding is omitted. Decoding tolerates (but does not require)
// padding to stay liberal with third-party tokens.

// Number of characters produced when base64url-encoding `binDataLen` bytes (no padding).
[[nodiscard]] constexpr std::size_t B64UrlEncodedLen(std::size_t binDataLen) noexcept {
  return ((binDataLen / 3U) * 4U) + (binDataLen % 3U == 0 ? 0 : (binDataLen % 3U) + 1U);
}

// Encode `binData` into `pOut`, which must have room for B64UrlEncodedLen(binData.size()) chars.
void B64UrlEncode(std::string_view binData, char* pOut) noexcept;

// Upper bound on the number of bytes produced by decoding `encodedLen` base64url characters.
[[nodiscard]] constexpr std::size_t B64UrlMaxDecodedLen(std::size_t encodedLen) noexcept {
  return ((encodedLen / 4U) * 3U) + 2U;
}

// Decode base64url `in` into `pOut` (sized via B64UrlMaxDecodedLen). Returns the number of bytes, or MAX if input is
// invalid. Optional '=' padding is accepted and ignored.
std::size_t B64UrlDecode(std::string_view in, char* pOut) noexcept;

}  // namespace aeronet
