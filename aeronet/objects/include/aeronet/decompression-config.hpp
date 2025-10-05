
#pragma once

#include <cstddef>

namespace aeronet {

// Request (inbound) body decompression configuration.
//
// Separate from outbound CompressionConfig to avoid bloating the public surface for users only
// interested in response compression and to make future hardening settings (ratio limits, allowlists)
// easier to evolve without breaking existing code.
struct DecompressionConfig {
  // Master enable flag. When false the server performs NO automatic decompression. Bodies with
  // Content-Encoding remain compressed and are delivered verbatim to handlers (pass-through).
  // No 415 is generated solely due to compression; application code may inspect/decode manually.
  // Default: enabled if any decoder is compiled in; disabled otherwise.
  bool enable{true};

  // Maximum compressed size (post framing decode, i.e. after chunked decoding) we are willing to
  // attempt to decompress. Protects against extremely large compressed blobs that would otherwise
  // waste CPU only to be rejected by downstream body size limits. 0 => no additional compressed
  // size specific cap (overall HttpServerConfig::maxBodyBytes still applies).
  std::size_t maxCompressedBytes{0};

  // Absolute cap on the decompressed size (in bytes). If exceeded during inflation, decompression
  // aborts and the request is rejected (413). Default: 1 GiB.
  std::size_t maxDecompressedBytes{1024UL * 1024UL * 1024UL};

  // Chunk size of buffer growths during decompression.
  // Prefer a large size if you expect big payloads in average, prefer a small size if you want to limit memory
  // overhead.
  std::size_t decoderChunkSize{32UL * 1024UL};

  // Ratio guard: if decompressed_size > compressed_size * maxExpansionRatio the request is
  // rejected (413) even if maxDecompressedBytes is not exceeded. This quickly rejects "compression
  // bombs" that expand massively but still under absolute byte cap if not configured tightly.
  // 0.0 => disabled.
  double maxExpansionRatio{0.0};
};

}  // namespace aeronet
