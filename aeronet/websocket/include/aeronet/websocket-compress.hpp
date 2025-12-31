#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/raw-bytes.hpp"

namespace aeronet {

/// WebSocket-specific compression context for permessage-deflate (RFC 7692).
/// This wraps zlib deflate/inflate with WebSocket-specific handling:
/// - Removes trailing 0x00 0x00 0xff 0xff from compressed data per RFC 7692 §7.2.1
/// - Appends the trailer back during decompression
/// - Supports context reset for no_context_takeover mode
class WebSocketCompressor {
 public:
  /// Create a WebSocket compressor with the specified compression level.
  /// @param compressionLevel zlib compression level (0-9)
  explicit WebSocketCompressor(int8_t compressionLevel);

  WebSocketCompressor(const WebSocketCompressor&) = delete;
  WebSocketCompressor& operator=(const WebSocketCompressor&) = delete;
  WebSocketCompressor(WebSocketCompressor&&) noexcept;
  WebSocketCompressor& operator=(WebSocketCompressor&&) noexcept;

  ~WebSocketCompressor();

  /// Compress a WebSocket message payload.
  /// @param input Uncompressed message data
  /// @param output Buffer to append compressed data to
  /// @param resetContext If true, reset the compression context before compressing
  /// @return true on success, false on compression error
  [[nodiscard]] bool compress(std::span<const std::byte> input, RawBytes& output, bool resetContext);

  /// Get last error message (if any).
  [[nodiscard]] std::string_view lastError() const noexcept { return _lastError; }

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
  std::string_view _lastError;
};

/// WebSocket-specific decompression context for permessage-deflate (RFC 7692).
class WebSocketDecompressor {
 public:
  WebSocketDecompressor();

  WebSocketDecompressor(const WebSocketDecompressor&) = delete;
  WebSocketDecompressor& operator=(const WebSocketDecompressor&) = delete;
  WebSocketDecompressor(WebSocketDecompressor&&) noexcept;
  WebSocketDecompressor& operator=(WebSocketDecompressor&&) noexcept;

  ~WebSocketDecompressor();

  /// Decompress a WebSocket message payload.
  /// @param input Compressed message data (without trailing 0x00 0x00 0xff 0xff)
  /// @param output Buffer to append decompressed data to
  /// @param maxDecompressedSize Maximum allowed decompressed size (0 = unlimited)
  /// @param resetContext If true, reset the decompression context before decompressing
  /// @return true on success, false on decompression error or size limit exceeded
  [[nodiscard]] bool decompress(std::span<const std::byte> input, RawBytes& output, std::size_t maxDecompressedSize,
                                bool resetContext);

  /// Get last error message (if any).
  [[nodiscard]] std::string_view lastError() const noexcept { return _lastError; }

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
  std::string_view _lastError;
};

}  // namespace aeronet
