#include "aeronet/base64.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aeronet {

namespace {

using B64Table = std::array<char, 64>;

// Build one of the two 64-symbol base64 alphabets from the 62 characters shared between
// standard base64 (RFC 4648 §4) and base64url (§5), plus the two symbols where they diverge.
constexpr B64Table MakeB64Table(char sym62, char sym63) noexcept {
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

constexpr B64Table kB64Table = MakeB64Table('+', '/');
constexpr B64Table kB64UrlTable = MakeB64Table('-', '_');

constexpr char* EncodeCore(std::string_view binData, char* out, const B64Table& table) noexcept {
  uint32_t bitsCollected{};
  uint32_t accumulator{};
  static constexpr uint32_t kNbBits = 6U;
  static constexpr uint32_t kMask6 = (1U << kNbBits) - 1U;
  for (char ch : binData) {
    accumulator = (accumulator << 8U) | static_cast<uint8_t>(ch);
    bitsCollected += 8U;
    while (bitsCollected >= kNbBits) {
      bitsCollected -= kNbBits;
      *out++ = table[(accumulator >> bitsCollected) & kMask6];
    }
  }
  if (bitsCollected > 0) {
    accumulator <<= (kNbBits - bitsCollected);
    *out++ = table[accumulator & kMask6];
  }
  return out;
}

}  // namespace

void B64Encode(std::string_view binData, char* out, const char* endOut) {
  out = EncodeCore(binData, out, kB64Table);
  while (out != endOut) {
    *out++ = '=';
  }
}

void B64UrlEncode(std::string_view binData, char* out) noexcept { EncodeCore(binData, out, kB64UrlTable); }

namespace {

// 6-bit value of a base64url symbol, or -1 when the character is not in the alphabet.
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

}  // namespace

std::size_t B64UrlDecode(std::string_view in, char* out) noexcept {
  // Trim optional padding so the remaining length drives the bit accounting.
  while (!in.empty() && in.back() == '=') {
    in.remove_suffix(1);
  }
  if (in.size() % 4U == 1) {
    return static_cast<std::size_t>(-1);  // a single leftover character carries only 6 bits - never valid
  }
  uint8_t bitsCollected{};
  uint32_t accumulator{};
  char* outStart = out;
  for (char ch : in) {
    const int val = B64UrlSextet(ch);
    if (val < 0) {
      return static_cast<std::size_t>(-1);
    }
    accumulator = (accumulator << 6U) | static_cast<uint32_t>(val);
    bitsCollected += 6U;
    if (bitsCollected >= 8U) {
      bitsCollected -= 8U;
      *out++ = static_cast<char>((accumulator >> bitsCollected) & 0xFFU);
    }
  }
  return static_cast<std::size_t>(out - outStart);
}

}  // namespace aeronet