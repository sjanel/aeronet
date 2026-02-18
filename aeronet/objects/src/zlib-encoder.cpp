#include "aeronet/zlib-encoder.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

int64_t ZlibEncoderContext::encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) {
  if (data.empty()) {
    return 0;
  }

  ZSetInput(_zs.stream, data);
  ZSetOutput(_zs.stream, buf, availableCapacity);

  const auto ret = ZDeflate(_zs.stream, Z_NO_FLUSH);
  if (ret == Z_STREAM_ERROR) [[unlikely]] {
    return -1;
  }

  if (_zs.stream.avail_in != 0) [[unlikely]] {
    return -1;
  }

  const std::size_t writtenNow = availableCapacity - _zs.stream.avail_out;
  return static_cast<int64_t>(writtenNow);
}

std::size_t ZlibEncoderContext::maxCompressedBytes(std::size_t uncompressedSize) const {
  return ZDeflateBound(const_cast<zstream*>(&_zs.stream), uncompressedSize);
}

int64_t ZlibEncoderContext::end(std::size_t availableCapacity, char* buf) noexcept {
  ZSetInput(_zs.stream, std::string_view{});
  ZSetOutput(_zs.stream, buf, availableCapacity);

  const int ret = ZDeflate(_zs.stream, Z_FINISH);
  if (ret == Z_STREAM_ERROR) {
    return -1;
  }

  const std::size_t writtenNow = availableCapacity - _zs.stream.avail_out;
  if (ret == Z_STREAM_END) {
    if (writtenNow == 0) {
      _zs.end();
      return 0;
    }
    return static_cast<int64_t>(writtenNow);
  }

  if (writtenNow == 0) {
    return -1;
  }

  return static_cast<int64_t>(writtenNow);
}

std::size_t ZlibEncoder::encodeFull(ZStreamRAII::Variant variant, std::string_view data, std::size_t availableCapacity,
                                    char* buf) {
  _ctx.init(_level, variant);

  auto& zstream = _ctx._zs.stream;

  ZSetInput(zstream, data);
  ZSetOutput(zstream, buf, availableCapacity);

  const auto rc = ZDeflate(zstream, Z_FINISH);
  std::size_t written = availableCapacity - zstream.avail_out;
  if (rc != Z_STREAM_END) {
    written = 0;
  } else {
    assert(zstream.avail_in == 0);
  }

  return written;
}

}  // namespace aeronet
