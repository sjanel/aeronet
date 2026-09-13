#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aeronet::http2 {

namespace detail {

// Generic big-endian read/write for native integer widths (16/32-bit).
// Compile-time evaluation uses portable shifts; runtime uses a raw load/store plus a conditional byteswap, which is a
// no-op on big-endian hosts.
template <typename T>
constexpr T ReadBE(const std::byte* data) noexcept {
  if consteval {
    T value{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      value = static_cast<T>((value << 8U) | static_cast<T>(data[i]));
    }
    return value;
  } else {
    T value;
    std::memcpy(&value, data, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
      value = std::byteswap(value);
    }
    return value;
  }
}

template <typename T>
constexpr void WriteBE(std::byte* data, T value) noexcept {
  if consteval {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      data[sizeof(T) - 1 - i] = static_cast<std::byte>(value & static_cast<T>(0xFFU));
      value = static_cast<T>(value >> 8U);
    }
  } else {
    if constexpr (std::endian::native == std::endian::little) {
      value = std::byteswap(value);
    }
    std::memcpy(data, &value, sizeof(T));
  }
}

}  // namespace detail

// Read a 16-bit big-endian value.
constexpr uint16_t Read16BE(const std::byte* data) noexcept { return detail::ReadBE<uint16_t>(data); }

// Read a 32-bit big-endian value.
constexpr uint32_t Read32BE(const std::byte* data) noexcept { return detail::ReadBE<uint32_t>(data); }

// Write a 16-bit big-endian value.
constexpr void Write16BE(std::byte* data, uint16_t value) noexcept { detail::WriteBE<uint16_t>(data, value); }

// Write a 32-bit big-endian value.
constexpr void Write32BE(std::byte* data, uint32_t value) noexcept { detail::WriteBE<uint32_t>(data, value); }

// Read a 24-bit big-endian value (returned in the low 24 bits of a uint32_t).
constexpr uint32_t Read24BE(const std::byte* data) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    // On a big-endian host, "value with top byte zero, low 3 bytes = data" is just data copied into the last 3 bytes of
    // a zeroed uint32_t: one fused load, no arithmetic, no over-read.
    if consteval {
      return (static_cast<uint32_t>(data[0]) << 16U) | (static_cast<uint32_t>(data[1]) << 8U) |
             static_cast<uint32_t>(data[2]);
    } else {
      uint32_t value = 0;
      std::memcpy(reinterpret_cast<std::byte*>(&value) + 1, data, 3);
      return value;
    }
  } else {
    // Little-endian (or non-standard) host: the equivalent trick would require reading a 4th byte we don't own, so fall
    // back to portable byte-by-byte assembly (works identically at compile time and runtime).
    return (static_cast<uint32_t>(data[0]) << 16U) | (static_cast<uint32_t>(data[1]) << 8U) |
           static_cast<uint32_t>(data[2]);
  }
}

// Write a 24-bit big-endian value (only the low 24 bits of `value` are used).
// No endian-dependent fast path exists here without writing a 4th byte we don't own, so this stays as plain byte stores
// on every architecture.
constexpr void Write24BE(std::byte* data, uint32_t value) noexcept {
  data[0] = static_cast<std::byte>((value >> 16U) & 0xFFU);
  data[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
  data[2] = static_cast<std::byte>(value & 0xFFU);
}

}  // namespace aeronet::http2