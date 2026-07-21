#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace aeronet {

constexpr void B64Encode(std::span<const char> binData, char* out, const char* endOut) {
  uint32_t bitsCollected{};
  uint32_t accumulator{};

  static constexpr char kB64Table[] = {
      'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
      'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
      's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/',
  };
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
  if (bitsCollected != 0) {
    accumulator <<= kB64NbBits - bitsCollected;
    *out++ = kB64Table[accumulator & kMask6];
  }
  while (out != endOut) {
    *out++ = '=';
  }
}

constexpr auto B64EncodedLen(auto binDataLen) { return static_cast<std::size_t>((binDataLen + 2) / 3) * 4; }

}  // namespace aeronet
