#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

/// WebSocket-specific compression context for permessage-deflate (RFC 7692).
/// This wraps zlib raw deflate/inflate (no zlib header nor checksum) with WebSocket-specific handling:
/// - Removes trailing 0x00 0x00 0xff 0xff from compressed data per RFC 7692 §7.2.1
/// - Appends the trailer back during decompression
/// - Supports context reset for no_context_takeover mode
/// The zlib state (about 300 KB for compression) is only allocated by the first compression.
class WebSocketCompressor {
 public:
  /// Create a WebSocket compressor with the specified compression level.
  /// @param compressionLevel deflate compression level (0-9)
  /// @param windowBits base two logarithm of the LZ77 window (9-15): the negotiated *_max_window_bits of the sender,
  /// raised to 9 when it is 8
  explicit WebSocketCompressor(int8_t compressionLevel, uint8_t windowBits = ZStreamRAII::kMaxWindowBits) noexcept
      : _level(compressionLevel), _windowBits(windowBits) {}

  /// Compress a WebSocket message payload.
  /// @param input Uncompressed message data
  /// @param output Buffer to append compressed data to
  /// @param resetContext If true, reset the compression context before compressing
  /// @return nullptr on success, error message on compression error
  [[nodiscard]] const char* compress(std::span<const std::byte> input, RawBytes& output, bool resetContext) {
    return compress(input, output, resetContext, kZMaxDeflateInputSize);
  }

 private:
  friend struct WebSocketCompressTestAccess;

  // zlib counts sizes in 32 bits: the input is given to deflate() in slices of at most maxSliceSize bytes.
  [[nodiscard]] const char* compress(std::span<const std::byte> input, RawBytes& output, bool resetContext,
                                     std::size_t maxSliceSize);

  ZStreamRAII _zs;
  int8_t _level;
  uint8_t _windowBits;
};

/// WebSocket-specific decompression context for permessage-deflate (RFC 7692).
/// The zlib state is only allocated by the first decompression.
class WebSocketDecompressor {
 public:
  /// @param windowBits base two logarithm of the LZ77 window (8-15): the negotiated *_max_window_bits of the peer
  explicit WebSocketDecompressor(uint8_t windowBits = ZStreamRAII::kMaxWindowBits) noexcept : _windowBits(windowBits) {}

  /// Decompress a WebSocket message payload.
  /// @param input Compressed message data (without trailing 0x00 0x00 0xff 0xff)
  /// @param output Buffer to append decompressed data to
  /// @param maxDecompressedSize Maximum allowed decompressed size
  /// @param resetContext If true, reset the decompression context before decompressing
  /// @return nullptr on success, error message on decompression error or size limit exceeded
  [[nodiscard]] const char* decompress(std::span<const std::byte> input, RawBytes& output,
                                       std::size_t maxDecompressedSize, bool resetContext) {
    return decompress(input, output, maxDecompressedSize, resetContext, kZMaxSize);
  }

 private:
  friend struct WebSocketCompressTestAccess;

  // zlib counts the input in 32 bits: it is given to inflate() in slices of at most maxSliceSize bytes.
  [[nodiscard]] const char* decompress(std::span<const std::byte> input, RawBytes& output,
                                       std::size_t maxDecompressedSize, bool resetContext, std::size_t maxSliceSize);

  ZStreamRAII _zs;
  uint8_t _windowBits;
};

}  // namespace aeronet
