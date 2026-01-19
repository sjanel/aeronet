#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::websocket {

/// Configuration for permessage-deflate extension (RFC 7692).
struct DeflateConfig {
  /// Whether to enable permessage-deflate compression (RFC 7692).
  /// If true and the client offers permessage-deflate, it will be negotiated.
#ifdef AERONET_ENABLE_ZLIB
  bool enabled{true};
#else
  bool enabled{false};
#endif

  /// Compression level (0 = no compression, 9 = best compression).
  /// Default is 6 (balanced speed/compression).
  int8_t compressionLevel{6};

  /// LZ77 sliding window size for compression (server's context).
  /// Valid values: 8-15 (representing 2^N bytes).
  /// Default 15 = 32KB window.
  uint8_t serverMaxWindowBits{15};

  /// LZ77 sliding window size for decompression (client's context).
  /// Valid values: 8-15 (representing 2^N bytes).
  /// Default 15 = 32KB window.
  uint8_t clientMaxWindowBits{15};

  /// If true, the server resets its compression context after each message.
  /// This uses more CPU but less memory.
  bool serverNoContextTakeover{false};

  /// If true, the client resets its compression context after each message.
  /// This uses more CPU but less memory.
  bool clientNoContextTakeover{false};

  /// Minimum message size to compress. Messages smaller than this are sent uncompressed.
  uint32_t minCompressSize{256U};
};

/// Negotiated permessage-deflate parameters (after upgrade handshake).
struct DeflateNegotiatedParams {
  uint8_t serverMaxWindowBits{15};
  uint8_t clientMaxWindowBits{15};
  bool serverNoContextTakeover{false};
  bool clientNoContextTakeover{false};
};

/// Parse a permessage-deflate extension offer from the client.
/// Returns negotiated parameters if the offer is acceptable, nullopt otherwise.
/// @param extensionOffer The extension offer string (e.g., "permessage-deflate; client_max_window_bits")
/// @param serverConfig Server's deflate configuration
/// @return Negotiated parameters or nullopt if cannot negotiate
[[nodiscard]] std::optional<DeflateNegotiatedParams> ParseDeflateOffer(std::string_view extensionOffer,
                                                                       const DeflateConfig& serverConfig);

/// Compute the size of the Sec-WebSocket-Extensions response header value for permessage-deflate.
std::size_t ComputeDeflateResponseSize(DeflateNegotiatedParams params);

/// Build the Sec-WebSocket-Extensions response header value for permessage-deflate.
/// @param params The negotiated parameters
/// @param output Output buffer to append the response string to
void BuildDeflateResponse(DeflateNegotiatedParams params, RawChars& output);

/// RAII wrapper for zlib deflate/inflate context.
/// This is an internal implementation detail.
class DeflateContext {
 public:
  /// Create a deflate context for compression/decompression.
  /// @param params Negotiated parameters from upgrade
  /// @param config Server's configuration (for compression level, etc.)
  /// @param isServerSide True for server, false for client
  DeflateContext(DeflateNegotiatedParams params, const DeflateConfig& config, bool isServerSide);

  DeflateContext(const DeflateContext&) = delete;
  DeflateContext& operator=(const DeflateContext&) = delete;
  DeflateContext(DeflateContext&&) noexcept = default;
  DeflateContext& operator=(DeflateContext&&) noexcept = default;

  ~DeflateContext();

  /// Compress a message payload.
  /// @param input Uncompressed message data
  /// @param output Buffer to append compressed data to
  /// @return nullptr on success, error message on compression error
  [[nodiscard]] const char* compress(std::span<const std::byte> input, RawBytes& output);

  /// Decompress a message payload.
  /// @param input Compressed message data
  /// @param output Buffer to append decompressed data to
  /// @param maxDecompressedSize Maximum allowed decompressed size (0 = unlimited)
  /// @return nullptr on success, error message on decompression error or size limit exceeded
  [[nodiscard]] const char* decompress(std::span<const std::byte> input, RawBytes& output,
                                       std::size_t maxDecompressedSize = 0);

  /// Check if compression should be skipped for a given payload size.
  [[nodiscard]] bool shouldSkipCompression(std::size_t payloadSize) const noexcept {
    return payloadSize < _minCompressSize;
  }

  using trivially_relocatable = std::true_type;

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
  uint32_t _minCompressSize{0};
};

}  // namespace aeronet::websocket
