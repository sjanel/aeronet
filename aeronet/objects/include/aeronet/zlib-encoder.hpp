#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/encoder-result.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#include "aeronet/zlib-variant.hpp"

namespace aeronet {

class ZlibEncoderContext final : public EncoderContext {
 public:
  ZlibEncoderContext() noexcept = default;

  ZlibEncoderContext(const ZlibEncoderContext&) = delete;
  ZlibEncoderContext(ZlibEncoderContext&& rhs) noexcept = default;
  ZlibEncoderContext& operator=(const ZlibEncoderContext&) = delete;
  ZlibEncoderContext& operator=(ZlibEncoderContext&& rhs) noexcept = default;

  ~ZlibEncoderContext() override = default;

  [[nodiscard]] std::size_t minEncodeChunkCapacity(std::size_t chunkSize) const override;

  EncoderResult encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) override;

  /// Initialize (or reinitialize) the compression context with given parameters.
  /// Reuses internal zlib state if already initialized.
  void init(int8_t level, ZlibVariant variant) { _zs.initCompress(variant, level); }

  [[nodiscard]] std::size_t endChunkSize() const override { return 64UL; }

  EncoderResult end(std::size_t availableCapacity, char* buf) noexcept override;

 private:
  friend class ZlibEncoder;
  friend struct ZlibTestAccess;

  // Deflates data into buf, the last input slice with `flush`. zlib counts sizes in 32 bits: the input is given in
  // slices of at most maxSliceSize bytes, the previous ones with Z_NO_FLUSH. Stops after the first slice deflate() does
  // not consume entirely. Returns the status of the last deflate() call, ZOutputEnd() of the stream ends the output.
  int deflateSlices(std::string_view data, int flush, std::size_t availableCapacity, char* buf,
                    std::size_t maxSliceSize = kZMaxDeflateInputSize);

  ZStreamRAII _zs;
};

class ZlibEncoder {
 public:
  explicit ZlibEncoder(int8_t level = 0) noexcept : _level(level) {}

  EncoderResult encodeFull(ZlibVariant variant, std::string_view data, std::size_t availableCapacity, char* buf);

  EncoderContext* makeContext(ZlibVariant variant) {
    _ctx.init(_level, variant);
    return &_ctx;
  }

  EncoderContext* context() { return &_ctx; }

 private:
  int8_t _level{};
  ZlibEncoderContext _ctx;
};

}  // namespace aeronet
