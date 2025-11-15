#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/internal/raw-bytes-base.hpp"

namespace aeronet {

// A byte buffer specialized for character data.
using RawChars = RawBytesBase<char, std::string_view, std::size_t>;

// A smaller version of RawChars using 32-bit size type for scenarios where memory usage matters.
// The maximum size is limited to 4 GiB in that case.
using SmallRawChars = RawBytesBase<char, std::string_view, std::uint32_t>;

}  // namespace aeronet