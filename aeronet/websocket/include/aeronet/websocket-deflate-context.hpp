#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/websocket-compress.hpp"
#include "aeronet/websocket-deflate.hpp"

namespace aeronet::websocket {

/// RAII wrapper for zlib deflate/inflate context.
/// This is an internal implementation detail.
class DeflateContext {
 public:
  /// Create a deflate context for compression/decompression.
  /// @param params Negotiated parameters from upgrade
  /// @param config Server's configuration (for compression level, etc.)
  /// @param isServerSide True for server, false for client
  /// @param sharedStreams If not null, streams used for the directions without context takeover instead of own ones.
  /// They must outlive this context.
  DeflateContext(DeflateNegotiatedParams params, const DeflateConfig& config, bool isServerSide,
                 SharedDeflateStreams* sharedStreams = nullptr);

  // Neither copyable nor movable: it may point to its own streams, which cannot move anyway.
  DeflateContext(const DeflateContext&) = delete;
  DeflateContext(DeflateContext&&) = delete;
  DeflateContext& operator=(const DeflateContext&) = delete;
  DeflateContext& operator=(DeflateContext&&) = delete;

  ~DeflateContext() = default;

  /// Compress a message payload.
  /// @param input Uncompressed message data
  /// @param output Buffer to append compressed data to
  /// @return nullptr on success, error message on compression error
  [[nodiscard]] const char* compress(std::span<const std::byte> input, RawBytes& output);

  /// Decompress a message payload.
  /// @param input Compressed message data
  /// @param output Buffer to append decompressed data to
  /// @param maxDecompressedSize Maximum allowed decompressed size: when exceeded, one byte more is appended to output
  /// @return nullptr on success, error message on decompression error or size limit exceeded
  [[nodiscard]] const char* decompress(std::span<const std::byte> input, RawBytes& output,
                                       std::size_t maxDecompressedSize = std::numeric_limits<std::size_t>::max());

  /// Check if compression should be skipped for a given payload size.
  [[nodiscard]] bool shouldSkipCompression(std::size_t payloadSize) const noexcept {
    return payloadSize < _minCompressSize;
  }

  /// Whether the compression continues its DEFLATE stream from message to message (context takeover): a message it
  /// compressed is in its window, which the next messages may refer to, so it must reach the peer compressed.
  [[nodiscard]] bool keepsCompressionContext() const noexcept { return !_deflateNoContextTakeover; }

 private:
  // Window bits and context takeover: own parameters when compressing, the peer's when decompressing.
  WebSocketCompressor _ownCompressor;
  WebSocketDecompressor _ownDecompressor;
  WebSocketCompressor* _compressor;
  WebSocketDecompressor* _decompressor;
  bool _deflateNoContextTakeover;
  bool _inflateNoContextTakeover;
  uint32_t _minCompressSize{0};
};

}  // namespace aeronet::websocket
