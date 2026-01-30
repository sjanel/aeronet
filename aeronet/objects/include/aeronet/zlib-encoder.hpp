#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

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

  int64_t encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) override;

  int64_t end(std::size_t availableCapacity, char* buf) noexcept override;

  /// Initialize (or reinitialize) the compression context with given parameters.
  /// Reuses internal zlib state if already initialized.
  void init(int8_t level, ZStreamRAII::Variant variant) { _zs.initCompress(variant, level); }

 private:
  friend class ZlibEncoder;

  ZStreamRAII _zs;
};

class ZlibEncoder {
 public:
  ZlibEncoder() noexcept = default;

  ZlibEncoder(ZStreamRAII::Variant variant, int8_t level) : _level(level), _variant(variant) {}

  std::size_t encodeFull(std::string_view data, std::size_t availableCapacity, char* buf);

  EncoderContext* makeContext() {
    _ctx.init(_level, _variant);
    return &_ctx;
  }

 private:
  int8_t _level{};
  ZStreamRAII::Variant _variant{ZStreamRAII::Variant::gzip};
  ZlibEncoderContext _ctx;
};

}  // namespace aeronet
