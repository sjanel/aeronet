#pragma once

#include <zlib.h>

#include <cstdint>

namespace aeronet {

struct ZStreamRAII {
  enum class Variant : int8_t { uninitialized, gzip, deflate };
  enum class Mode : int8_t { uninitialized, compress, decompress };

  // Default constructor - leaves stream uninitialized.
  ZStreamRAII() noexcept = default;

  // Initialize a z_stream for decompression.
  // Throws std::runtime_error on failure.
  explicit ZStreamRAII(Variant variant);

  // Initialize a z_stream for compression.
  // Throws std::runtime_error on failure.
  ZStreamRAII(Variant variant, int8_t level) { initCompress(variant, level); }

  // z_stream is not moveable or copyable if allocated - but we authorize all these only if stream is not initialized.
  ZStreamRAII(const ZStreamRAII& rhs) = delete;
  ZStreamRAII(ZStreamRAII&& rhs) noexcept;
  ZStreamRAII& operator=(const ZStreamRAII& rhs) = delete;
  ZStreamRAII& operator=(ZStreamRAII&& rhs) noexcept;

  ~ZStreamRAII() { end(); }

  /// Initialize (or reinitialize) a z_stream for compression.
  /// Reuses internal state if already initialized for compression.
  void initCompress(Variant variant, int8_t level);

  void end() noexcept;

  z_stream stream;

 private:
  Variant _variant{Variant::uninitialized};
  Mode _mode{Mode::uninitialized};
  int8_t _level{};
};

}  // namespace aeronet