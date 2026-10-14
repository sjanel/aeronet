#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aeronet {

/// Manages buffer allocation and size limits for streaming decompression.
/// Grows the buffer by chunks and bounds the room given to the decoder to one byte beyond the limit, so that an output
/// exceeding it is detected exactly (limitExceeded()), whatever capacity the buffer already has.
template <class ByteBuffer>
class DecoderBufferManager {
 public:
  /// Create a buffer manager.
  /// @param buf Reference to the buffer being filled with decompressed data
  /// @param decoderChunkSize Minimal growth of the buffer
  /// @param maxDecompressedBytes Maximum number of bytes to append to buf (std::numeric_limits<std::size_t>::max():
  /// unlimited)
  DecoderBufferManager(ByteBuffer& buf, uint32_t decoderChunkSize, std::size_t maxDecompressedBytes)
      : _buf(buf),
        // Saturated so that one byte beyond the limit stays representable.
        _maxSize(buf.size() + std::min<std::size_t>(maxDecompressedBytes,
                                                    std::numeric_limits<std::size_t>::max() - 1U - buf.size())),
        _decoderChunkSize(decoderChunkSize) {}

  /// Reserve room for the next chunk of decompressed data and return its size: the number of bytes the decoder may
  /// write at the end of the buffer, at least one. It extends one byte beyond the limit.
  /// Must not be called once the limit is exceeded.
  std::size_t nextReserve() {
    assert(!limitExceeded());
    const std::size_t sizeCap = _maxSize + 1;
    const std::size_t desired = std::min<std::size_t>(_buf.size() + _decoderChunkSize, sizeCap);

    // Only grow when we actually need more capacity.
    if (_buf.capacity() < desired) {
      _buf.reserve(std::min(std::max<std::size_t>(desired, _buf.capacity() * 2UL), sizeCap));
    }
    return std::min<std::size_t>(_buf.capacity(), sizeCap) - _buf.size();
  }

  /// Whether more bytes than maxDecompressedBytes were appended to the buffer.
  [[nodiscard]] bool limitExceeded() const noexcept { return _buf.size() > _maxSize; }

 private:
  ByteBuffer& _buf;
  std::size_t _maxSize;
  uint32_t _decoderChunkSize;
};

}  // namespace aeronet
