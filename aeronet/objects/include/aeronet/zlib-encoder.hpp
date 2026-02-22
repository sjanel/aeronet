#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/encoder-result.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

class ZlibEncoderContext final : public EncoderContext {
 public:
  ZlibEncoderContext() noexcept = default;

  ZlibEncoderContext(const ZlibEncoderContext&) = delete;
  ZlibEncoderContext(ZlibEncoderContext&& rhs) noexcept = default;
  ZlibEncoderContext& operator=(const ZlibEncoderContext&) = delete;
  ZlibEncoderContext& operator=(ZlibEncoderContext&& rhs) noexcept = default;

  ~ZlibEncoderContext() override = default;

  [[nodiscard]] std::size_t maxCompressedBytes(std::size_t uncompressedSize) const override;

  [[nodiscard]] std::size_t endChunkSize() const override { return 64UL; }

  EncoderResult encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) override;

  /// Initialize (or reinitialize) the compression context with given parameters.
  /// Reuses internal zlib state if already initialized.
  void init(int8_t level, ZStreamRAII::Variant variant) { _zs.initCompress(variant, level); }

  EncoderResult end(std::size_t availableCapacity, char* buf) noexcept override;

 private:
  friend class ZlibEncoder;

  ZStreamRAII _zs;
};

class ZlibEncoder {
 public:
  ZlibEncoder() noexcept = default;

  explicit ZlibEncoder(int8_t level) : _level(level) {}

  EncoderResult encodeFull(ZStreamRAII::Variant variant, std::string_view data, std::size_t availableCapacity,
                           char* buf);

  EncoderContext* makeContext(ZStreamRAII::Variant variant) {
    _ctx.init(_level, variant);
    return &_ctx;
  }

  EncoderContext* context() { return &_ctx; }

 private:
  int8_t _level{};
  ZlibEncoderContext _ctx;
};

}  // namespace aeronet
