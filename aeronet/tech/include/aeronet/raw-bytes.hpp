#pragma once

#include <cstddef>
#include <span>

#include "aeronet/internal/raw-bytes-base.hpp"

namespace aeronet {

// A byte buffer.
using RawBytes = RawBytesBase<std::byte, std::span<const std::byte>, std::size_t>;

}  // namespace aeronet