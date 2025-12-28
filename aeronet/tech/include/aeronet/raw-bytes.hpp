#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "aeronet/internal/raw-bytes-base.hpp"

namespace aeronet {

// A byte buffer.
using RawBytes = RawBytesBase<std::byte, std::span<const std::byte>, std::size_t>;

// A byte buffer with 32-bit size type.
using RawBytes32 = RawBytesBase<std::byte, std::span<const std::byte>, std::uint32_t>;

}  // namespace aeronet