/*
    sha1.hpp - SHA-1 implementation

    Based on public domain code from https://github.com/vog/sha1

    Original C Code
        -- Steve Reid <steve@edmweb.com>
    Small changes to fit into bglibs
        -- Bruce Guenter <bruce@untroubled.org>
    Translation to simpler C++ Code
        -- Volker Diels-Grabsch <v@njh.eu>
    Safety fixes
        -- Eugene Hopkinson <slowriot at voxelstorm dot com>
    Header-only library
        -- Zlatko Michailov <zlatko@michailov.org>
    Modernized (no allocations, std::string_view input, std::array output)
        -- aeronet contributors
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aeronet {

/// Raw 20-byte SHA-1 digest
using Sha1Digest = std::array<char, 20>;

/// Incremental SHA-1 hasher. Call update() one or more times, then final() to get the digest.
class SHA1 {
 public:
  SHA1() noexcept { reset(); }

  /// Reset hasher to initial state (allows reuse)
  void reset() noexcept;

  /// Feed data into the hasher (can be called multiple times)
  void update(const char* data, std::size_t sz) noexcept;

  /// Finalize and return the 20-byte digest. Resets internal state for reuse.
  [[nodiscard]] Sha1Digest final() noexcept;

 private:
  static constexpr std::size_t kBlockInts = 16;
  static constexpr std::size_t kBlockBytes = kBlockInts * 4;

  static uint32_t rol(uint32_t value, std::size_t bits) noexcept { return (value << bits) | (value >> (32 - bits)); }

  static uint32_t blk(const uint32_t block[kBlockInts], std::size_t idx) noexcept {
    return rol(block[(idx + 13) & 15] ^ block[(idx + 8) & 15] ^ block[(idx + 2) & 15] ^ block[idx], 1);
  }

  static void R0(const uint32_t block[kBlockInts], uint32_t vv, uint32_t& ww, uint32_t xx, uint32_t yy, uint32_t& zz,
                 std::size_t idx) noexcept {
    zz += ((ww & (xx ^ yy)) ^ yy) + block[idx] + 0x5a827999 + rol(vv, 5);
    ww = rol(ww, 30);
  }

  static void R1(uint32_t block[kBlockInts], uint32_t vv, uint32_t& ww, uint32_t xx, uint32_t yy, uint32_t& zz,
                 std::size_t idx) noexcept {
    block[idx] = blk(block, idx);
    zz += ((ww & (xx ^ yy)) ^ yy) + block[idx] + 0x5a827999 + rol(vv, 5);
    ww = rol(ww, 30);
  }

  static void R2(uint32_t block[kBlockInts], uint32_t vv, uint32_t& ww, uint32_t xx, uint32_t yy, uint32_t& zz,
                 std::size_t idx) noexcept {
    block[idx] = blk(block, idx);
    zz += (ww ^ xx ^ yy) + block[idx] + 0x6ed9eba1 + rol(vv, 5);
    ww = rol(ww, 30);
  }

  static void R3(uint32_t block[kBlockInts], uint32_t vv, uint32_t& ww, uint32_t xx, uint32_t yy, uint32_t& zz,
                 std::size_t idx) noexcept {
    block[idx] = blk(block, idx);
    zz += (((ww | xx) & yy) | (ww & xx)) + block[idx] + 0x8f1bbcdc + rol(vv, 5);
    ww = rol(ww, 30);
  }

  static void R4(uint32_t block[kBlockInts], uint32_t vv, uint32_t& ww, uint32_t xx, uint32_t yy, uint32_t& zz,
                 std::size_t idx) noexcept {
    block[idx] = blk(block, idx);
    zz += (ww ^ xx ^ yy) + block[idx] + 0xca62c1d6 + rol(vv, 5);
    ww = rol(ww, 30);
  }

  void transform(uint32_t block[kBlockInts]) noexcept;
  void bufferToBlock(uint32_t block[kBlockInts]) const noexcept;

  uint32_t _digest[5];
  std::array<char, kBlockBytes> _buffer;
  std::size_t _bufferSize;
  uint64_t _transforms;
};

// ============================================================================
// Inline implementation
// ============================================================================

inline void SHA1::reset() noexcept {
  _digest[0] = 0x67452301;
  _digest[1] = 0xefcdab89;
  _digest[2] = 0x98badcfe;
  _digest[3] = 0x10325476;
  _digest[4] = 0xc3d2e1f0;
  _bufferSize = 0;
  _transforms = 0;
}

inline void SHA1::transform(uint32_t block[kBlockInts]) noexcept {
  uint32_t aa = _digest[0];
  uint32_t bb = _digest[1];
  uint32_t cc = _digest[2];
  uint32_t dd = _digest[3];
  uint32_t ee = _digest[4];

  R0(block, aa, bb, cc, dd, ee, 0);
  R0(block, ee, aa, bb, cc, dd, 1);
  R0(block, dd, ee, aa, bb, cc, 2);
  R0(block, cc, dd, ee, aa, bb, 3);
  R0(block, bb, cc, dd, ee, aa, 4);
  R0(block, aa, bb, cc, dd, ee, 5);
  R0(block, ee, aa, bb, cc, dd, 6);
  R0(block, dd, ee, aa, bb, cc, 7);
  R0(block, cc, dd, ee, aa, bb, 8);
  R0(block, bb, cc, dd, ee, aa, 9);
  R0(block, aa, bb, cc, dd, ee, 10);
  R0(block, ee, aa, bb, cc, dd, 11);
  R0(block, dd, ee, aa, bb, cc, 12);
  R0(block, cc, dd, ee, aa, bb, 13);
  R0(block, bb, cc, dd, ee, aa, 14);
  R0(block, aa, bb, cc, dd, ee, 15);
  R1(block, ee, aa, bb, cc, dd, 0);
  R1(block, dd, ee, aa, bb, cc, 1);
  R1(block, cc, dd, ee, aa, bb, 2);
  R1(block, bb, cc, dd, ee, aa, 3);
  R2(block, aa, bb, cc, dd, ee, 4);
  R2(block, ee, aa, bb, cc, dd, 5);
  R2(block, dd, ee, aa, bb, cc, 6);
  R2(block, cc, dd, ee, aa, bb, 7);
  R2(block, bb, cc, dd, ee, aa, 8);
  R2(block, aa, bb, cc, dd, ee, 9);
  R2(block, ee, aa, bb, cc, dd, 10);
  R2(block, dd, ee, aa, bb, cc, 11);
  R2(block, cc, dd, ee, aa, bb, 12);
  R2(block, bb, cc, dd, ee, aa, 13);
  R2(block, aa, bb, cc, dd, ee, 14);
  R2(block, ee, aa, bb, cc, dd, 15);
  R2(block, dd, ee, aa, bb, cc, 0);
  R2(block, cc, dd, ee, aa, bb, 1);
  R2(block, bb, cc, dd, ee, aa, 2);
  R2(block, aa, bb, cc, dd, ee, 3);
  R2(block, ee, aa, bb, cc, dd, 4);
  R2(block, dd, ee, aa, bb, cc, 5);
  R2(block, cc, dd, ee, aa, bb, 6);
  R2(block, bb, cc, dd, ee, aa, 7);
  R3(block, aa, bb, cc, dd, ee, 8);
  R3(block, ee, aa, bb, cc, dd, 9);
  R3(block, dd, ee, aa, bb, cc, 10);
  R3(block, cc, dd, ee, aa, bb, 11);
  R3(block, bb, cc, dd, ee, aa, 12);
  R3(block, aa, bb, cc, dd, ee, 13);
  R3(block, ee, aa, bb, cc, dd, 14);
  R3(block, dd, ee, aa, bb, cc, 15);
  R3(block, cc, dd, ee, aa, bb, 0);
  R3(block, bb, cc, dd, ee, aa, 1);
  R3(block, aa, bb, cc, dd, ee, 2);
  R3(block, ee, aa, bb, cc, dd, 3);
  R3(block, dd, ee, aa, bb, cc, 4);
  R3(block, cc, dd, ee, aa, bb, 5);
  R3(block, bb, cc, dd, ee, aa, 6);
  R3(block, aa, bb, cc, dd, ee, 7);
  R3(block, ee, aa, bb, cc, dd, 8);
  R3(block, dd, ee, aa, bb, cc, 9);
  R3(block, cc, dd, ee, aa, bb, 10);
  R3(block, bb, cc, dd, ee, aa, 11);
  R4(block, aa, bb, cc, dd, ee, 12);
  R4(block, ee, aa, bb, cc, dd, 13);
  R4(block, dd, ee, aa, bb, cc, 14);
  R4(block, cc, dd, ee, aa, bb, 15);
  R4(block, bb, cc, dd, ee, aa, 0);
  R4(block, aa, bb, cc, dd, ee, 1);
  R4(block, ee, aa, bb, cc, dd, 2);
  R4(block, dd, ee, aa, bb, cc, 3);
  R4(block, cc, dd, ee, aa, bb, 4);
  R4(block, bb, cc, dd, ee, aa, 5);
  R4(block, aa, bb, cc, dd, ee, 6);
  R4(block, ee, aa, bb, cc, dd, 7);
  R4(block, dd, ee, aa, bb, cc, 8);
  R4(block, cc, dd, ee, aa, bb, 9);
  R4(block, bb, cc, dd, ee, aa, 10);
  R4(block, aa, bb, cc, dd, ee, 11);
  R4(block, ee, aa, bb, cc, dd, 12);
  R4(block, dd, ee, aa, bb, cc, 13);
  R4(block, cc, dd, ee, aa, bb, 14);
  R4(block, bb, cc, dd, ee, aa, 15);

  _digest[0] += aa;
  _digest[1] += bb;
  _digest[2] += cc;
  _digest[3] += dd;
  _digest[4] += ee;

  ++_transforms;
}

inline void SHA1::bufferToBlock(uint32_t block[kBlockInts]) const noexcept {
  for (std::size_t idx = 0; idx < kBlockInts; ++idx) {
    block[idx] = static_cast<uint32_t>(static_cast<unsigned char>(_buffer[(4 * idx) + 3])) |
                 static_cast<uint32_t>(static_cast<unsigned char>(_buffer[(4 * idx) + 2])) << 8 |
                 static_cast<uint32_t>(static_cast<unsigned char>(_buffer[(4 * idx) + 1])) << 16 |
                 static_cast<uint32_t>(static_cast<unsigned char>(_buffer[(4 * idx) + 0])) << 24;
  }
}

inline void SHA1::update(const char* data, std::size_t sz) noexcept {
  while (sz > 0) {
    std::size_t toCopy = kBlockBytes - _bufferSize;
    // NOLINTNEXTLINE(readability-use-std-min-max)
    if (toCopy > sz) {
      toCopy = sz;
    }
    std::memcpy(_buffer.data() + _bufferSize, data, toCopy);
    _bufferSize += toCopy;
    data += toCopy;
    sz -= toCopy;

    if (_bufferSize == kBlockBytes) {
      uint32_t block[kBlockInts];
      bufferToBlock(block);
      transform(block);
      _bufferSize = 0;
    }
  }
}

inline Sha1Digest SHA1::final() noexcept {
  const uint64_t totalBits = (_transforms * kBlockBytes + _bufferSize) * 8;

  // Append padding byte
  _buffer[_bufferSize] = static_cast<char>(0x80);
  ++_bufferSize;

  // Pad to 64 bytes
  std::size_t origSize = _bufferSize;
  while (_bufferSize < kBlockBytes) {
    _buffer[_bufferSize] = 0;
    ++_bufferSize;
  }

  uint32_t block[kBlockInts];
  bufferToBlock(block);

  // If we don't have room for the length, transform and start a new block
  if (origSize > kBlockBytes - 8) {
    transform(block);
    std::memset(block, 0, (kBlockInts - 2) * sizeof(uint32_t));
  }

  // Append total bit count (big-endian)
  block[kBlockInts - 1] = static_cast<uint32_t>(totalBits);
  block[kBlockInts - 2] = static_cast<uint32_t>(totalBits >> 32);
  transform(block);

  // Convert digest to bytes (big-endian)
  Sha1Digest result;
  for (std::size_t idx = 0; idx < 5; ++idx) {
    result[(idx * 4) + 0] = static_cast<char>((_digest[idx] >> 24) & 0xff);
    result[(idx * 4) + 1] = static_cast<char>((_digest[idx] >> 16) & 0xff);
    result[(idx * 4) + 2] = static_cast<char>((_digest[idx] >> 8) & 0xff);
    result[(idx * 4) + 3] = static_cast<char>(_digest[idx] & 0xff);
  }

  reset();
  return result;
}

}  // namespace aeronet
