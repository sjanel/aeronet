#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>

namespace aeronet {

/// Manages buffer allocation and size limits for streaming decompression.
/// Ensures decompressed size never exceeds the specified maximum by controlling
/// buffer growth and signaling when the limit would be exceeded.
template <class ByteBuffer>
class DecoderBufferManager {
 public:
  /// Create a buffer manager.
  /// @param buf Reference to the buffer being filled with decompressed data
  /// @param decoderChunkSize Size of chunks processed at a time
  /// @param maxDecompressedBytes Maximum allowed decompressed size (0 = unlimited)
  DecoderBufferManager(ByteBuffer& buf, std::size_t decoderChunkSize, std::size_t maxDecompressedBytes)
      : _buf(buf),
        _decoderChunkSize(decoderChunkSize),
        _maxDecompressedBytes(maxDecompressedBytes),
        _initialSize(buf.size()) {
    if (_maxDecompressedBytes == 0) {
      _maxDecompressedBytes = std::numeric_limits<std::size_t>::max() - _initialSize;
    }
  }

  /// Reserve space for the next chunk and check if we should stop.
  /// @return true if the next chunk would exceed the size limit, false otherwise
  bool nextReserve() {
    const auto alreadyDecompressed = _buf.size() - _initialSize;
    const bool forceEnd = alreadyDecompressed + _decoderChunkSize > _maxDecompressedBytes;
    const std::size_t desired = _buf.size() + _decoderChunkSize;

    // Only grow when we actually need more capacity.
    if (_buf.capacity() < desired) {
      std::size_t capacity = _initialSize + _maxDecompressedBytes;
      // If reached the maximum allowed decompressed size - force end if current chunk does not reach the end.
      if (!forceEnd) {
        const std::size_t doubled = _buf.capacity() * 2UL;
        capacity = std::min(std::max(desired, doubled), capacity);
      }
      _buf.reserve(capacity);
    }
    return forceEnd;
  }

 private:
  ByteBuffer& _buf;
  std::size_t _decoderChunkSize;
  std::size_t _maxDecompressedBytes;
  std::size_t _initialSize;
};

}  // namespace aeronet
