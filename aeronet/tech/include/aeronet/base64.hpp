#pragma once

#include <cstddef>
#include <string_view>

namespace aeronet {

constexpr auto B64EncodedLen(auto binDataLen) { return static_cast<std::size_t>((binDataLen + 2) / 3) * 4; }

// Encode `binData` into `out`, which must have room for B64EncodedLen(binData.size()) chars.
// Pads with '=' until 'endOut.
void B64Encode(std::string_view binData, char* out, const char* endOut);

// Base64url (RFC 4648 §5) without padding — the encoding mandated by JOSE/JWT (RFC 7515 §2,
// RFC 7519). Differs from standard base64 in two ways: the alphabet uses '-' and '_' instead of
// '+' and '/', and the trailing '=' padding is omitted. Decoding tolerates (but does not require)
// padding to stay liberal with third-party tokens.

// Number of characters produced when base64url-encoding `binDataLen` bytes (no padding).
[[nodiscard]] constexpr std::size_t B64UrlEncodedLen(std::size_t binDataLen) noexcept {
  return ((binDataLen / 3) * 4) + (binDataLen % 3 == 0 ? 0 : (binDataLen % 3) + 1);
}

// Encode `binData` into `out`, which must have room for B64UrlEncodedLen(binData.size()) chars.
void B64UrlEncode(std::string_view binData, char* out) noexcept;

// Upper bound on the number of bytes produced by decoding `encodedLen` base64url characters.
[[nodiscard]] constexpr std::size_t B64UrlMaxDecodedLen(std::size_t encodedLen) noexcept {
  return ((encodedLen / 4) * 3) + 2;
}

// Decode base64url `in` into `out` (sized via B64UrlMaxDecodedLen). Returns the number of bytes, or MAX if input is
// invalid. Optional '=' padding is accepted and ignored.
std::size_t B64UrlDecode(std::string_view in, char* out) noexcept;

}  // namespace aeronet
