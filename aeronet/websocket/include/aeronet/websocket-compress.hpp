#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

/// WebSocket-specific compression context for permessage-deflate (RFC 7692).
/// This wraps zlib deflate/inflate with WebSocket-specific handling:
/// - Removes trailing 0x00 0x00 0xff 0xff from compressed data per RFC 7692 §7.2.1
/// - Appends the trailer back during decompression
/// - Supports context reset for no_context_takeover mode
class WebSocketCompressor {
 public:
  /// Create a WebSocket compressor with the specified compression level.
  /// @param compressionLevel deflate compression level (0-9)
  explicit WebSocketCompressor(int8_t compressionLevel);

  /// Compress a WebSocket message payload.
  /// @param input Uncompressed message data
  /// @param output Buffer to append compressed data to
  /// @param resetContext If true, reset the compression context before compressing
  /// @return nullptr on success, error message on compression error
  [[nodiscard]] const char* compress(std::span<const std::byte> input, RawBytes& output, bool resetContext);

 private:
  ZStreamRAII _zs;
};

/// WebSocket-specific decompression context for permessage-deflate (RFC 7692).
class WebSocketDecompressor {
 public:
  WebSocketDecompressor() = default;

  /// Decompress a WebSocket message payload.
  /// @param input Compressed message data (without trailing 0x00 0x00 0xff 0xff)
  /// @param output Buffer to append decompressed data to
  /// @param maxDecompressedSize Maximum allowed decompressed size (0 = unlimited)
  /// @param resetContext If true, reset the decompression context before decompressing
  /// @return nullptr on success, error message on decompression error or size limit exceeded
  [[nodiscard]] const char* decompress(std::span<const std::byte> input, RawBytes& output,
                                       std::size_t maxDecompressedSize, bool resetContext);

 private:
  ZStreamRAII _zs{ZStreamRAII::Variant::deflate};
};

}  // namespace aeronet
