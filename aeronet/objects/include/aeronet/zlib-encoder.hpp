#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/encoder.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

class ZlibEncoderContext final : public EncoderContext {
 public:
  ZlibEncoderContext() noexcept = default;

  explicit ZlibEncoderContext(RawChars& sharedBuf) : _pBuf(&sharedBuf) {}

  ZlibEncoderContext(const ZlibEncoderContext&) = delete;
  ZlibEncoderContext(ZlibEncoderContext&& rhs) noexcept = default;
  ZlibEncoderContext& operator=(const ZlibEncoderContext&) = delete;
  ZlibEncoderContext& operator=(ZlibEncoderContext&& rhs) noexcept = default;

  ~ZlibEncoderContext() override = default;

  std::string_view encodeChunk(std::string_view chunk) override;

  /// Initialize (or reinitialize) the compression context with given parameters.
  /// Reuses internal zlib state if already initialized.
  void init(int8_t level, ZStreamRAII::Variant variant) { _zs.initCompress(variant, level); }

  void end() noexcept { _zs.end(); }

 private:
  friend class ZlibEncoder;

  RawChars* _pBuf{nullptr};
  ZStreamRAII _zs;
};

class ZlibEncoder {
 public:
  ZlibEncoder() noexcept = default;

  ZlibEncoder(ZStreamRAII::Variant variant, RawChars& buf, int8_t level)
      : _level(level), _variant(variant), _ctx(buf) {}

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
