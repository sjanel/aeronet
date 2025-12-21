#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/internal/raw-bytes-base.hpp"

namespace aeronet {

// A byte buffer specialized for character data.
// Uses std::string_view as view type and std::uint64_t as size type.
using RawChars = RawBytesBase<char, std::string_view, std::uint64_t>;

// A smaller version of RawChars using 32-bit size type for scenarios where memory footprint matters.
// The maximum size is limited to 4 GiB in that case.
using RawChars32 = RawBytesBase<char, std::string_view, std::uint32_t>;

}  // namespace aeronet