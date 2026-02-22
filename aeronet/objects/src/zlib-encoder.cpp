#include "aeronet/zlib-encoder.hpp"

#include <cassert>
#include <cstddef>
#include <string_view>

#include "aeronet/encoder-result.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

EncoderResult ZlibEncoderContext::encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) {
  assert(!data.empty());

  ZSetInput(_zs.stream, data);
  ZSetOutput(_zs.stream, buf, availableCapacity);

  const auto ret = ZDeflate(_zs.stream, Z_NO_FLUSH);
  if (ret == Z_STREAM_ERROR || _zs.stream.avail_in != 0) [[unlikely]] {
    return EncoderResult(EncoderResult::Error::CompressionError);
  }

  return EncoderResult(availableCapacity - _zs.stream.avail_out);
}

std::size_t ZlibEncoderContext::maxCompressedBytes(std::size_t uncompressedSize) const {
  return ZDeflateBound(const_cast<zstream*>(&_zs.stream), uncompressedSize);
}

EncoderResult ZlibEncoderContext::end(std::size_t availableCapacity, char* buf) noexcept {
  ZSetInput(_zs.stream, std::string_view{});
  ZSetOutput(_zs.stream, buf, availableCapacity);

  const int ret = ZDeflate(_zs.stream, Z_FINISH);
  if (ret == Z_STREAM_ERROR) {
    return EncoderResult(EncoderResult::Error::CompressionError);
  }

  const std::size_t writtenNow = availableCapacity - _zs.stream.avail_out;
  if (ret == Z_STREAM_END) {
    if (writtenNow == 0) {
      _zs.end();
    }
    return EncoderResult(writtenNow);
  }

  if (writtenNow == 0) {
    return EncoderResult(EncoderResult::Error::CompressionError);
  }

  return EncoderResult(writtenNow);
}

EncoderResult ZlibEncoder::encodeFull(ZStreamRAII::Variant variant, std::string_view data,
                                      std::size_t availableCapacity, char* buf) {
  _ctx.init(_level, variant);

  auto& zstream = _ctx._zs.stream;

  ZSetInput(zstream, data);
  ZSetOutput(zstream, buf, availableCapacity);

  const auto rc = ZDeflate(zstream, Z_FINISH);
  const std::size_t written = availableCapacity - zstream.avail_out;

  if (rc != Z_STREAM_END) {
    if (rc == Z_OK) {
      return EncoderResult(EncoderResult::Error::NotEnoughCapacity);
    }
    return EncoderResult(EncoderResult::Error::CompressionError);
  }
  assert(zstream.avail_in == 0);

  return EncoderResult(written);
}

}  // namespace aeronet
