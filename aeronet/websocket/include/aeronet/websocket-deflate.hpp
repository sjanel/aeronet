#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/websocket-shared-deflate-streams.hpp"  // IWYU pragma: export

namespace aeronet::websocket {

/// Configuration for permessage-deflate extension (RFC 7692).
struct DeflateConfig {
  void validate() const;

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
  /// Valid values: 8-15 (representing 2^N bytes). zlib cannot compress raw DEFLATE data with a 256 bytes window: with
  /// 8, the server compresses with 9 bits, whose matches never refer more than 250 bytes back.
  /// Default 15 = 32KB window.
  uint8_t serverMaxWindowBits{15};

  /// LZ77 sliding window size for decompression (client's context).
  /// Valid values: 8-15 (representing 2^N bytes). Only applied to the clients offering client_max_window_bits: the
  /// others keep a 32KB window (RFC 7692 section 7.1.2.2).
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
  [[nodiscard]] bool hasValue() const noexcept { return serverMaxWindowBits != 0; }

  void setEmpty() { serverMaxWindowBits = 0; }

  uint8_t serverMaxWindowBits{0};
  uint8_t clientMaxWindowBits{15};
  bool serverNoContextTakeover{false};
  bool clientNoContextTakeover{false};
};

/// Parse a permessage-deflate extension offer from the client.
/// Returns negotiated parameters if the offer is acceptable, nullopt otherwise.
/// @param extensionOffer The extension offer string (e.g., "permessage-deflate; client_max_window_bits")
/// @param serverConfig Server's deflate configuration
/// @return Negotiated parameters or nullopt if cannot negotiate
[[nodiscard]] DeflateNegotiatedParams ParseDeflateOffer(std::string_view extensionOffer,
                                                        const DeflateConfig& serverConfig);

/// Compute the size of the Sec-WebSocket-Extensions response header value for permessage-deflate.
std::size_t ComputeDeflateResponseSize(DeflateNegotiatedParams params, uint8_t nDigitsServerMaxWindowBits,
                                       uint8_t nDigitsClientMaxWindowBits);

/// Build the Sec-WebSocket-Extensions response header value for permessage-deflate.
/// pData should have at least ComputeDeflateResponseSize size available.
/// @param params The negotiated parameters
/// @param pData Output buffer to append the response string to
char* BuildDeflateResponse(DeflateNegotiatedParams params, char* pData);

}  // namespace aeronet::websocket
