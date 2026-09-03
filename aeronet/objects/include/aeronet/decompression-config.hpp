
#pragma once

#include <cstddef>
#include <cstdint>

namespace aeronet {

// Request (inbound) body decompression configuration.
//
// Separate from outbound CompressionConfig to avoid bloating the public surface for users only
// interested in response compression and to make future hardening settings (ratio limits, allowlists)
// easier to evolve without breaking existing code.
struct DecompressionConfig {
  void validate() const;

// Master enable flag. When false the server performs NO automatic decompression. Bodies with
// Content-Encoding remain compressed and are delivered verbatim to handlers (pass-through).
// No 415 is generated solely due to compression; application code may inspect/decode manually.
// Default: enabled if any decoder is compiled in; disabled otherwise.
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  bool enable{true};
#else
  bool enable{false};
#endif

  // Minimal chunk size of buffer growths during decompression.
  // Prefer a large size if you expect big payloads in average, prefer a small size if you want to limit memory
  // overhead. Note that the growth will be exponential anyway.
  uint32_t decoderChunkSize{32U << 10U};

  // Maximum compressed size (post framing decode, i.e. after chunked decoding) we are willing to attempt to decompress.
  // Protects against extremely large compressed blobs that would otherwise waste CPU only to be rejected by downstream
  // body size limits. Checked against the compressed byte count before decompression begins.
  // Default: 128 MiB.
  std::size_t maxCompressedBytes{128U << 20U};

  // Absolute cap on the decompressed size (in bytes). If exceeded during inflation, decompression aborts and the
  // request is rejected (413).
  // Default: 4 GiB.
  std::size_t maxDecompressedBytes{4ULL << 30U};

  // When Content-Length is greater or equal to this threshold (bytes), inbound decompression switches to streaming
  // contexts to avoid allocating full intermediate buffers for large payloads.
  // Defaults to 16 MiB.
  std::size_t streamingDecompressionThresholdBytes{16U << 20U};

  // Ratio guard: if decompressed_size > compressed_size * maxExpansionRatio the request is rejected (413) even if
  // maxDecompressedBytes is not exceeded. Catches "compression bombs" that expand massively while still under the
  // absolute byte cap. Legitimate compressible content (text, JSON, logs) rarely exceeds ~30-50x even when highly
  // redundant; bombs routinely exceed 1000x.
  // Default: 1000.0 (1000x).
  double maxExpansionRatio{1000.0};
};

}  // namespace aeronet
