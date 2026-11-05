#include "aeronet/base64.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aeronet {

namespace {

constexpr B64Table kB64UrlTable = MakeB64Table('-', '_');

constexpr char* EncodeCore(std::string_view binData, char* pOut, const B64Table& table) noexcept {
  uint32_t bitsCollected{};
  uint32_t accumulator{};
  static constexpr uint32_t kNbBits = 6U;
  static constexpr uint32_t kMask6 = (1U << kNbBits) - 1U;
  for (char ch : binData) {
    accumulator = (accumulator << 8U) | static_cast<uint8_t>(ch);
    bitsCollected += 8U;
    while (bitsCollected >= kNbBits) {
      bitsCollected -= kNbBits;
      *pOut++ = table[(accumulator >> bitsCollected) & kMask6];
    }
  }
  if (bitsCollected > 0) {
    accumulator <<= (kNbBits - bitsCollected);
    *pOut++ = table[accumulator & kMask6];
  }
  return pOut;
}

}  // namespace

void B64Encode(std::string_view binData, char* pOut, const char* pEndOut) {
  pOut = EncodeCore(binData, pOut, kB64Table);
  while (pOut != pEndOut) {
    *pOut++ = '=';
  }
}

void B64UrlEncode(std::string_view binData, char* pOut) noexcept { EncodeCore(binData, pOut, kB64UrlTable); }

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

std::size_t B64UrlDecode(std::string_view in, char* pOut) noexcept {
  // Trim optional padding so the remaining length drives the bit accounting.
  in = in.substr(0, in.find_last_not_of('=') + 1);
  if (in.size() % 4U == 1) {
    return static_cast<std::size_t>(-1);  // a single leftover character carries only 6 bits - never valid
  }
  uint8_t bitsCollected{};
  uint32_t accumulator{};
  char* outStart = pOut;
  for (char ch : in) {
    const int val = B64UrlSextet(ch);
    if (val < 0) {
      return static_cast<std::size_t>(-1);
    }
    accumulator = (accumulator << 6U) | static_cast<uint32_t>(val);
    bitsCollected += 6U;
    if (bitsCollected >= 8U) {
      bitsCollected = static_cast<uint8_t>(bitsCollected - 8U);
      *pOut++ = static_cast<char>((accumulator >> bitsCollected) & 0xFFU);
    }
  }
  return static_cast<std::size_t>(pOut - outStart);
}

}  // namespace aeronet