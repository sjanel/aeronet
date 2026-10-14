#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aeronet::http2 {

namespace detail {

// Generic big-endian read/write for native integer widths (16/32/64-bit).
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

// Read a 64-bit big-endian value.
constexpr uint64_t Read64BE(const std::byte* data) noexcept { return detail::ReadBE<uint64_t>(data); }

// Write a 16-bit big-endian value.
constexpr void Write16BE(std::byte* data, uint16_t value) noexcept { detail::WriteBE<uint16_t>(data, value); }

// Write a 32-bit big-endian value.
constexpr void Write32BE(std::byte* data, uint32_t value) noexcept { detail::WriteBE<uint32_t>(data, value); }

// Write a 64-bit big-endian value.
constexpr void Write64BE(std::byte* data, uint64_t value) noexcept { detail::WriteBE<uint64_t>(data, value); }

}  // namespace aeronet::http2