#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

class ZlibEncoderContext final : public EncoderContext {
 public:
  ZlibEncoderContext(ZStreamRAII::Variant variant, RawChars& sharedBuf, int8_t level);

  std::string_view encodeChunk(std::string_view chunk) override;

 private:
  RawChars& _buf;
  ZStreamRAII _zs;
};

class ZlibEncoder {
 public:
  ZlibEncoder() noexcept = default;

  ZlibEncoder(ZStreamRAII::Variant variant, RawChars& buf, const CompressionConfig& cfg)
      : pBuf(&buf), _level(cfg.zlib.level), _variant(variant) {}

  std::size_t encodeFull(std::string_view data, std::size_t availableCapacity, char* buf);

  std::unique_ptr<EncoderContext> makeContext() {
    return std::make_unique<ZlibEncoderContext>(_variant, *pBuf, _level);
  }

 private:
  RawChars* pBuf{nullptr};  // shared output buffer reused (single-thread guarantee)
  int8_t _level{};
  ZStreamRAII::Variant _variant{ZStreamRAII::Variant::gzip};
};

}  // namespace aeronet
