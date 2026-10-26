#pragma once

#include <cstdint>

#include "aeronet/buffer-cache.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-variant.hpp"

namespace aeronet {

class ZStreamRAII {
 public:
  // Base two logarithm of the default (and maximum) window size.
  static constexpr uint8_t kMaxWindowBits = 15;

  // Default constructor - leaves stream uninitialized.
  ZStreamRAII() noexcept = default;

  // Initialize a z_stream for decompression.
  // Throws std::runtime_error on failure.
  explicit ZStreamRAII(ZlibVariant variant) { initDecompress(variant); }

  // Initialize a z_stream for compression.
  // Throws std::runtime_error on failure.
  ZStreamRAII(ZlibVariant variant, int8_t level) { initCompress(variant, level); }

  // The zlib state is bound to the address of its z_stream (zlib refuses a moved one) and of the cache allocating it:
  // it cannot move. A move only hands over the cached memory, leaving both streams uninitialized (initialized again by
  // their next init*() call).
  ZStreamRAII(const ZStreamRAII& rhs) = delete;
  ZStreamRAII(ZStreamRAII&& rhs) noexcept;
  ZStreamRAII& operator=(const ZStreamRAII& rhs) = delete;
  ZStreamRAII& operator=(ZStreamRAII&& rhs) noexcept;

  ~ZStreamRAII() { end(); }

  /// Initialize (or reinitialize) a z_stream for compression, with a window of 2^windowBits bytes (9-15).
  /// Reuses internal state if already initialized for compression.
  void initCompress(ZlibVariant variant, int8_t level, uint8_t windowBits = kMaxWindowBits);

  // Initialize (or reinitialize) a z_stream for decompression, with a window of 2^windowBits bytes (8-15).
  // Reuses internal state if already initialized for decompression.
  void initDecompress(ZlibVariant variant, uint8_t windowBits = kMaxWindowBits);

  void end() noexcept;

  /// Tells whether the z_stream is initialized (for compression or decompression).
  [[nodiscard]] bool initialized() const noexcept { return _mode != Mode::uninitialized; }

  zstream stream{};

 private:
  enum class Mode : int8_t { uninitialized, compress, decompress };

  void initZcache();

  internal::BufferCache _cache;
  ZlibVariant _variant{ZlibVariant::uninitialized};
  Mode _mode{Mode::uninitialized};
  int8_t _level{};
  uint8_t _windowBits{kMaxWindowBits};
};

}  // namespace aeronet