#pragma once

#include <cstdint>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

// Format a file size into a human-readable string using binary units (powers of 1024).
// Rules:
//  - Units used: B, KiB, MiB, GiB, TiB, PiB, EiB (where 1 KiB == 1024 bytes, 1 MiB == 1024*1024 bytes, ...).
//  - For values < 1024 bytes the function prints an integer number of bytes, e.g. "512 B".
//  - For values >= 1024 the value is divided by 1024 repeatedly to find the largest unit
//    with a value < 1024. For those units we print a decimal number; formatting uses one
//    decimal place when the numeric value is less than 10 (to preserve a single significant
//    fractional digit) and no decimals when the value is >= 10. Examples:
//      0         -> "0 B"
//      512       -> "512 B"
//      1536      -> "1.5 KiB"   (1536 / 1024 == 1.5)
//      1048576   -> "1.0 MiB"   (1024*1024 -> value is 1.0, one decimal is shown)
//      12345678  -> "11.8 MiB"  (approx; displays one decimal when < 10, otherwise no fractional)
//  - A single space separates the number and the unit (e.g. "1.5 KiB").
void AddFormattedSize(std::uintmax_t size, RawChars& out);

}  // namespace aeronet