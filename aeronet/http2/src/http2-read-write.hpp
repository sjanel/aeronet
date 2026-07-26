#pragma once

#include <cstddef>
#include <cstdint>

namespace aeronet::http2 {

// Read a 24-bit big-endian value.
constexpr uint32_t Read24BE(const std::byte* data) {
  return (static_cast<uint32_t>(data[0]) << 16) | (static_cast<uint32_t>(data[1]) << 8) |
         static_cast<uint32_t>(data[2]);
}

// Read a 32-bit big-endian value.
constexpr uint32_t Read32BE(const std::byte* data) noexcept {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

// Write a 24-bit big-endian value.
constexpr void Write24BE(std::byte* data, uint32_t value) noexcept {
  data[0] = static_cast<std::byte>((value >> 16) & 0xFF);
  data[1] = static_cast<std::byte>((value >> 8) & 0xFF);
  data[2] = static_cast<std::byte>(value & 0xFF);
}

// Write a 32-bit big-endian value.
constexpr void Write32BE(std::byte* data, uint32_t value) noexcept {
  data[0] = static_cast<std::byte>((value >> 24) & 0xFF);
  data[1] = static_cast<std::byte>((value >> 16) & 0xFF);
  data[2] = static_cast<std::byte>((value >> 8) & 0xFF);
  data[3] = static_cast<std::byte>(value & 0xFF);
}

// Write a 16-bit big-endian value.
constexpr void Write16BE(std::byte* data, uint16_t value) noexcept {
  data[0] = static_cast<std::byte>((value >> 8) & 0xFF);
  data[1] = static_cast<std::byte>(value & 0xFF);
}

// Read a 16-bit big-endian value.
constexpr uint16_t Read16BE(const std::byte* data) noexcept {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]));
}

}  // namespace aeronet::http2