#pragma once

#include <cstdint>

#include "aeronet/buffer-cache.hpp"
#include "aeronet/zlib-gateway.hpp"

namespace aeronet {

struct ZStreamRAII {
  // gzip: gzip wrapper (RFC 1952). deflate: zlib wrapper (RFC 1950), the HTTP 'deflate' content-coding.
  // raw: raw DEFLATE stream without wrapper (RFC 1951), as used by WebSocket permessage-deflate (RFC 7692).
  enum class Variant : int8_t { uninitialized, gzip, deflate, raw };
  enum class Mode : int8_t { uninitialized, compress, decompress };

  // Base two logarithm of the default (and maximum) window size.
  static constexpr uint8_t kMaxWindowBits = 15;

  // Default constructor - leaves stream uninitialized.
  ZStreamRAII() noexcept = default;

  // Initialize a z_stream for decompression.
  // Throws std::runtime_error on failure.
  explicit ZStreamRAII(Variant variant) { initDecompress(variant); }

  // Initialize a z_stream for compression.
  // Throws std::runtime_error on failure.
  ZStreamRAII(Variant variant, int8_t level) { initCompress(variant, level); }

  // z_stream is not moveable or copyable if allocated - but we authorize all these only if stream is not initialized.
  ZStreamRAII(const ZStreamRAII& rhs) = delete;
  ZStreamRAII(ZStreamRAII&& rhs) noexcept;
  ZStreamRAII& operator=(const ZStreamRAII& rhs) = delete;
  ZStreamRAII& operator=(ZStreamRAII&& rhs) noexcept;

  ~ZStreamRAII() { end(); }

  /// Initialize (or reinitialize) a z_stream for compression, with a window of 2^windowBits bytes (9-15).
  /// Reuses internal state if already initialized for compression.
  void initCompress(Variant variant, int8_t level, uint8_t windowBits = kMaxWindowBits);

  // Initialize (or reinitialize) a z_stream for decompression, with a window of 2^windowBits bytes (8-15).
  // Reuses internal state if already initialized for decompression.
  void initDecompress(Variant variant, uint8_t windowBits = kMaxWindowBits);

  void end() noexcept;

  zstream stream;

 private:
  void initZcache();

  internal::BufferCache _cache;
  Variant _variant{Variant::uninitialized};
  Mode _mode{Mode::uninitialized};
  int8_t _level{};
  uint8_t _windowBits{kMaxWindowBits};
};

}  // namespace aeronet