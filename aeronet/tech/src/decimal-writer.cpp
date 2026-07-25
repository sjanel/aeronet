#include "aeronet/decimal-writer.hpp"

#include <cassert>
#include <cstdint>

#include "aeronet/ndigits.hpp"
#include "aeronet/simple-charconv.hpp"

namespace aeronet {

char* WriteU16(char* out, uint16_t val, uint8_t nbDigits) noexcept {
  assert(nbDigits >= 1 && nbDigits <= 5);

  const uint32_t v32 = static_cast<uint32_t>(val);
  const uint32_t v1 = v32 / 100;         // 0..655
  const uint32_t r1 = v32 - (v1 * 100);  // last two digits, 0..99
  const uint32_t v2 = v1 / 100;          // 0..6   (ten-thousands digit)
  const uint32_t r2 = v1 - (v2 * 100);   // thousands/hundreds, 0..99
  switch (nbDigits) {
    case 1:
      out[0] = static_cast<char>('0' + v32);  // v < 10 here
      break;
    case 2:
      write2(out, r1);  // r1 == v here
      break;
    case 3:
      out[0] = static_cast<char>('0' + r2);  // r2 < 10 here
      write2(out + 1, r1);
      break;
    case 4:
      write2(out, r2);  // r2 < 100 here
      write2(out + 2, r1);
      break;
    default:  // 5
      out[0] = static_cast<char>('0' + v2);
      write2(out + 1, r2);
      write2(out + 3, r1);
      break;
  }
  return out + nbDigits;
}

// Writes in base 10, padded with leading zeros to exactly len digits.
char* WriteU32(char* out, uint32_t val, uint8_t nbDigits) noexcept {
  assert(nbDigits >= ndigits(val));
  char* ptr = out + nbDigits;
  while (val >= 100) {
    ptr -= 2;
    write2(ptr, val % 100);
    val /= 100;
  }
  if (val < 10) {
    *--ptr = static_cast<char>('0' + val);
  } else {
    ptr -= 2;
    write2(ptr, val);
  }
  return out + nbDigits;
}

namespace {

// Writes a 32-bit value in base 10, padded with leading zeros to exactly 9 digits.
constexpr char* Write9Padded(char* out, uint32_t val) noexcept {
  char* ptr = out + 9;
  for (int i = 0; i < 4; ++i) {
    ptr -= 2;
    write2(ptr, val % 100);
    val /= 100;
  }
  *--ptr = static_cast<char>('0' + val);  // only one digit left, 0 <= val < 10
  return out + 9;
}

}  // namespace

char* WriteU64(char* out, uint64_t val, uint8_t nbDigits) noexcept {
  // Fast path: the vast majority of real values (ports, sizes, content-length, timestamps in seconds...) fits in 32
  // bits.
  assert(nbDigits >= ndigits(val));
  if (val <= 0xFFFFFFFFULL) {
    return WriteU32(out, static_cast<uint32_t>(val), nbDigits);
  }

  static constexpr uint64_t kBase = 1000000000ULL;  // 10^9

  const uint64_t low = val % kBase;
  val /= kBase;

  if (val < kBase) {
    // x now fits in a uint32_t: 2 blocks are enough.
    char* ptr = WriteU32(out, static_cast<uint32_t>(val), ndigits(static_cast<uint32_t>(val)));
    return Write9Padded(ptr, static_cast<uint32_t>(low));
  }

  // 3 blocks needed : high (<= 18, so 1 or 2 digits), mid, low.
  const uint64_t mid = val % kBase;
  const uint64_t high = val / kBase;

  char* ptr = WriteU32(out, static_cast<uint32_t>(high), ndigits(static_cast<uint32_t>(high)));
  ptr = Write9Padded(ptr, static_cast<uint32_t>(mid));
  return Write9Padded(ptr, static_cast<uint32_t>(low));
}

}  // namespace aeronet
