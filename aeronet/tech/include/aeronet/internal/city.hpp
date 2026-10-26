// Copyright (c) 2011 Google, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// CityHash, by Geoff Pike and Jyrki Alakuijala
//
// http://code.google.com/p/cityhash/
//
// This file provides the CityHash32 and CityHash64 hash functions (see city_impl.hpp). They are
// high-quality functions in the sense that they pass standard tests such as Austin Appleby's SMHasher.
// They are also fast.
//
// For 64-bit x86 code, on short strings, we don't know of anything faster than
// CityHash64 that is of comparable quality.  We believe our nearest competitor
// is Murmur3.  For 64-bit x86 code, CityHash64 is an excellent choice for hash
// tables and most other hashing (excluding cryptography).
//
// For 32-bit x86 code, we don't know of anything faster than CityHash32 that
// is of comparable quality.  We believe our nearest competitor is Murmur3A.
//
// Functions in the CityHash family are not suitable for cryptography.
//
// WARNING: This code has been only lightly tested on big-endian platforms!
// It is known to work well on little-endian platforms that have a small penalty
// for unaligned reads, such as current Intel and AMD moderate-to-high-end CPUs.
// It should work on all 32-bit and 64-bit platforms that allow unaligned reads;
// bug reports are welcome.
//
// By the way, for some hash functions, given strings a and b, the hash
// of a+b is easily derived from the hashes of a and b.  This property
// doesn't hold for any hash functions in this file.

// Modifications for aeronet only - some constexpr added here and there, and the unused variants
// (seeded, 128-bit and CRC ones) removed.

#ifndef CITY_HASH_H_
#define CITY_HASH_H_

#include <cstdint>

using uint8 = uint8_t;
using uint32 = uint32_t;
using uint64 = uint64_t;

// Hash 128 input bits (given as two 64-bit halves) down to 64 bits of output.
// This is intended to be a reasonably good hash function.
constexpr uint64 Hash128to64(uint64 low, uint64 high) {
  // Murmur-inspired hashing.
  static constexpr uint64 kMul = 0x9ddfea08eb382d69ULL;
  uint64 lhs = (low ^ high) * kMul;
  lhs ^= (lhs >> 47U);
  uint64 rhs = (high ^ lhs) * kMul;
  rhs ^= (rhs >> 47U);
  rhs *= kMul;
  return rhs;
}

#endif  // CITY_HASH_H_

#include "city_impl.hpp"  // IWYU pragma: export
