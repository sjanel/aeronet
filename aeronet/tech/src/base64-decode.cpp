#include "aeronet/base64-decode.hpp"

#include <span>
#include <stdexcept>
#include <string>

#include "aeronet/cctype.hpp"

namespace aeronet {

std::string B64Decode(std::span<const char> ascData) {
  std::string ret;
  ret.reserve((ascData.size() * 3) / 4);  // Max possible size
  int bitsCollected = 0;
  unsigned int accumulator = 0;

  static constexpr unsigned char kReverseTable[] = {
      64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
      64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63, 52, 53, 54, 55,
      56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64, 64, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
      13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64, 64, 26, 27, 28, 29, 30, 31, 32,
      33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64};

  for (char ch : ascData) {
    if (isspace(ch) || ch == '=') {
      // Skip whitespace and padding
      continue;
    }
    if (ch < 0 || kReverseTable[static_cast<unsigned char>(ch)] > 63) {
      throw std::invalid_argument("Illegal character detected for a base 64 encoded string");
    }
    accumulator = (accumulator << 6) | kReverseTable[static_cast<unsigned char>(ch)];
    bitsCollected += 6;
    if (bitsCollected >= 8) {
      bitsCollected -= 8;
      ret.push_back(static_cast<char>((accumulator >> bitsCollected) & 0xFFU));
    }
  }
  return ret;
}

}  // namespace aeronet
