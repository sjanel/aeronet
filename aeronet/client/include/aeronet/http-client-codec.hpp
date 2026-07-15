#pragma once

#include "aeronet/compression-config.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::internal {

// Compression / decompression state for one HttpClient: the inbound (response) decoders and the
// outbound (request) encoders plus their scratch buffers, all reused across requests so the per-call
// cost is just the (de)compression itself. Mirrors the server's per-session codec state and reuses the
// exact same bricks (DecompressionState / CompressionState / HttpCodec).
struct HttpClientCodec {
  explicit HttpClientCodec(const CompressionConfig& compressionCfg) : compressionState(compressionCfg) {}

  DecompressionState decompressionState;  // inbound response decoders
  CompressionState compressionState;      // outbound request encoders
  RawChars decompressOut;                 // decoded response body (ping-pong primary)
  RawChars decompressTmp;                 // scratch for stacked-encoding decode
};

}  // namespace aeronet::internal
