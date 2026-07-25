#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace aeronet {

// Writes the decimal representation of `value` starting at `out` (without a null terminator), and returns a pointer
// just after the last character written. The buffer pointed to by `out` must contain at least kMaxDigitsUxx bytes
// available from `out`: no checks are performed.
char* WriteU16(char* out, uint16_t val, uint8_t nbDigits) noexcept;
char* WriteU32(char* out, uint32_t val, uint8_t nbDigits) noexcept;
char* WriteU64(char* out, uint64_t val, uint8_t nbDigits) noexcept;

// Generic dispatcher for unsigned integral types, to avoid code duplication in the caller.
inline char* WriteUInt(char* out, std::unsigned_integral auto val, uint8_t nbDigits) noexcept {
  if constexpr (sizeof(decltype(val)) <= sizeof(uint16_t)) {
    return WriteU16(out, static_cast<uint16_t>(val), nbDigits);
  } else if constexpr (sizeof(decltype(val)) <= sizeof(uint32_t)) {
    return WriteU32(out, static_cast<uint32_t>(val), nbDigits);
  } else {
    return WriteU64(out, static_cast<uint64_t>(val), nbDigits);
  }
}

// Signed wrapper: compute absolute value safely in unsigned type to avoid
// overflow for minimum value and delegate to unsigned implementation.
inline char* WriteSInt(char* out, std::signed_integral auto val, uint8_t nbDigits) noexcept {
  using U = std::make_unsigned_t<decltype(val)>;

  if (val >= 0) {
    return WriteUInt(out, static_cast<U>(val), nbDigits);
  }

  *out++ = '-';

  // Use -(val+1) which is representable, then add 1 after casting to unsigned.
  return WriteUInt(out, static_cast<U>(-(val + 1)) + 1U, nbDigits);
}

// Generic dispatcher for signed or unsigned integral types, to avoid code duplication in the caller.
inline char* WriteInt(char* out, std::integral auto val, uint8_t nbDigits) noexcept {
  if constexpr (std::is_signed_v<decltype(val)>) {
    return WriteSInt(out, val, nbDigits);
  } else {
    return WriteUInt(out, val, nbDigits);
  }
}

}  // namespace aeronet
