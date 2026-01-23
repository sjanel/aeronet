#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "aeronet/tolower-str.hpp"
#include "city.hpp"

namespace aeronet {

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif

inline uint64_t CityHash64CI(const char* str, std::size_t len) {
  static constexpr std::size_t kChunk = 8 * sizeof(uint64_t);
  uint64_t seed0 = City::k2;
  uint64_t seed1 = City::k1;

  char buf[kChunk];
  std::size_t offset = 0;

  // Process full chunks
  while (offset + kChunk <= len) {
    // Fold ASCII to lowercase
    for (std::size_t chunkPos = 0; chunkPos < kChunk; chunkPos += sizeof(uint64_t)) {
      uint64_t val;
      std::memcpy(&val, str + offset + chunkPos, sizeof(uint64_t));
      val = AsciiLowerMask(val);
      std::memcpy(buf + chunkPos, &val, sizeof(uint64_t));
    }

    uint64_t hash = City::CityHash64WithSeeds(buf, kChunk, seed0, seed1);
    seed0 ^= hash;
    seed1 += hash;
    offset += kChunk;
  }

  // Tail (<= 63 bytes)
  if (offset < len) {
    std::size_t tail = len - offset;
    tolower_n(str + offset, tail, buf);
    uint64_t hash = City::CityHash64WithSeeds(buf, tail, seed0, seed1);
    seed0 ^= hash;
    seed1 += hash;
  }

  return City::HashLen16(seed0, seed1);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

}  // namespace aeronet