#pragma once

#include <cstdint>
#include <string_view>

#include "raw-bytes.hpp"

namespace aeronet {

// A byte buffer specialized for character data.
using RawChars = RawBytesImpl<char, std::string_view>;

// A smaller version of RawChars using 32-bit size type for scenarios where memory usage matters.
// The maximum size is limited to 4 GiB in that case.
using SmallRawChars = RawBytesImpl<char, std::string_view, std::uint32_t>;

}  // namespace aeronet