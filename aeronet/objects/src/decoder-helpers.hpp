#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

class DecoderBufferManager {
 public:
  DecoderBufferManager(RawChars &buf, std::size_t decoderChunkSize, std::size_t maxDecompressedBytes)
      : _buf(buf),
        _decoderChunkSize(decoderChunkSize),
        _maxDecompressedBytes(maxDecompressedBytes),
        _initialSize(buf.size()) {
    if (_maxDecompressedBytes == 0) {
      _maxDecompressedBytes = std::numeric_limits<std::size_t>::max() - _initialSize;
    }
  }

  bool nextReserve() {
    const auto alreadyDecompressed = _buf.size() - _initialSize;
    const bool forceEnd = alreadyDecompressed + _decoderChunkSize > _maxDecompressedBytes;
    const std::size_t desired = _buf.size() + _decoderChunkSize;

    // Only grow when we actually need more capacity.
    if (_buf.capacity() < desired) {
      std::size_t capacity = _initialSize + _maxDecompressedBytes;
      // If reached the maximum allowed decompressed size - force end if current chunk does not reach the end.
      if (!forceEnd) {
        const std::size_t doubled = (_buf.capacity() * 2UL) + 1UL;
        capacity = std::min(std::max(desired, doubled), capacity);
      }
      _buf.reserve(capacity);
    }
    return forceEnd;
  }

 private:
  RawChars &_buf;
  std::size_t _decoderChunkSize;
  std::size_t _maxDecompressedBytes;
  std::size_t _initialSize;
};

}  // namespace aeronet