#pragma once

#include <zlib.h>

#include <cstdint>

namespace aeronet {

struct ZStreamRAII {
  enum class Variant : int8_t { gzip, deflate };
  enum class Type : int8_t { compress, decompress };

  // Initialize a z_stream for decompression.
  // Throws std::runtime_error on failure.
  explicit ZStreamRAII(Variant variant);

  // Initialize a z_stream for compression.
  // Throws std::runtime_error on failure.
  ZStreamRAII(Variant variant, int8_t level);

  // z_stream is not moveable or copyable - delete these operations
  ZStreamRAII(const ZStreamRAII&) = delete;
  ZStreamRAII(ZStreamRAII&&) noexcept = delete;
  ZStreamRAII& operator=(const ZStreamRAII&) = delete;
  ZStreamRAII& operator=(ZStreamRAII&&) noexcept = delete;

  ~ZStreamRAII();

  z_stream stream{};

 private:
  bool _isDeflate;
};

}  // namespace aeronet