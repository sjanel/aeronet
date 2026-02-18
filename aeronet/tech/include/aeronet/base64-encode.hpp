#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aeronet {

constexpr void B64Encode(std::span<const char> binData, char* out, const char* endOut) {
  int bitsCollected{};
  uint32_t accumulator{};

  static constexpr const char kB64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static constexpr auto kB64NbBits = 6;
  static constexpr decltype(accumulator) kMask6 = (1U << kB64NbBits) - 1U;

  for (char ch : binData) {
    accumulator = (accumulator << 8) | static_cast<uint8_t>(ch);
    bitsCollected += 8;
    while (bitsCollected >= kB64NbBits) {
      bitsCollected -= kB64NbBits;
      *out++ = kB64Table[(accumulator >> bitsCollected) & kMask6];
    }
  }
  if (bitsCollected > 0) {
    accumulator <<= kB64NbBits - bitsCollected;
    *out++ = kB64Table[accumulator & kMask6];
  }
  while (out != endOut) {
    *out++ = '=';
  }
}

constexpr auto B64EncodedLen(auto binDataLen) { return static_cast<std::size_t>((binDataLen + 2) / 3) * 4; }

template <std::size_t N>
[[nodiscard]] constexpr auto B64Encode(const char (&binData)[N]) {
  std::array<char, B64EncodedLen(N)> ret;
  B64Encode(binData, ret.data(), ret.data() + ret.size());
  return ret;
}

template <std::size_t N>
[[nodiscard]] constexpr auto B64Encode(const std::array<char, N>& binData) {
  std::array<char, B64EncodedLen(N)> ret;
  B64Encode(binData, ret.data(), ret.data() + ret.size());
  return ret;
}

}  // namespace aeronet
