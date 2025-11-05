#pragma once

#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "encoder.hpp"
#include "raw-chars.hpp"

namespace aeronet {

namespace details {

struct ZStreamRAII {
  enum class Variant : int8_t { gzip, deflate };

  ZStreamRAII(Variant variant, int8_t level);

  ZStreamRAII(const ZStreamRAII&) = delete;
  ZStreamRAII(ZStreamRAII&&) noexcept = delete;
  ZStreamRAII& operator=(const ZStreamRAII&) = delete;
  ZStreamRAII& operator=(ZStreamRAII&&) noexcept = delete;

  ~ZStreamRAII();

  z_stream _stream{};
};
}  // namespace details

class ZlibEncoderContext : public EncoderContext {
 public:
  ZlibEncoderContext(details::ZStreamRAII::Variant variant, RawChars& sharedBuf, int8_t level);

  std::string_view encodeChunk(std::size_t encoderChunkSize, std::string_view chunk) override;

 private:
  RawChars& _buf;
  bool _finished{false};
  details::ZStreamRAII _zs;
};

class ZlibEncoder : public Encoder {
 public:
  explicit ZlibEncoder(details::ZStreamRAII::Variant variant, const CompressionConfig& cfg,
                       std::size_t initialCapacity = 4096UL)
      : _buf(initialCapacity), _level(cfg.zlib.level), _variant(variant) {}

  std::string_view encodeFull(std::size_t encoderChunkSize, std::string_view in) override {
    return compressAll(encoderChunkSize, in);
  }

  std::unique_ptr<EncoderContext> makeContext() override {
    return std::make_unique<ZlibEncoderContext>(_variant, _buf, _level);
  }

 private:
  std::string_view compressAll(std::size_t encoderChunkSize, std::string_view in);

  RawChars _buf;  // shared output buffer reused (single-thread guarantee)
  int8_t _level;
  details::ZStreamRAII::Variant _variant;
};

}  // namespace aeronet
