#include "aeronet/zlib-encoder.hpp"

#include <zconf.h>
#include <zlib.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

int64_t ZlibEncoderContext::encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) {
  if (data.empty()) {
    return 0;
  }

  _zs.stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  _zs.stream.avail_in = static_cast<uInt>(data.size());

  _zs.stream.next_out = reinterpret_cast<unsigned char*>(buf);
  _zs.stream.avail_out = static_cast<decltype(_zs.stream.avail_out)>(availableCapacity);

  const auto ret = deflate(&_zs.stream, Z_NO_FLUSH);
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
  return deflateBound(const_cast<z_stream*>(&_zs.stream), static_cast<uLong>(uncompressedSize));
}

int64_t ZlibEncoderContext::end(std::size_t availableCapacity, char* buf) noexcept {
  _zs.stream.next_in = nullptr;
  _zs.stream.avail_in = 0;

  _zs.stream.next_out = reinterpret_cast<unsigned char*>(buf);
  _zs.stream.avail_out = static_cast<decltype(_zs.stream.avail_out)>(availableCapacity);

  const int ret = deflate(&_zs.stream, Z_FINISH);
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

std::size_t ZlibEncoder::encodeFull(std::string_view data, std::size_t availableCapacity, char* buf) {
  _ctx.init(_level, _variant);

  auto& zstream = _ctx._zs.stream;

  zstream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  zstream.avail_in = static_cast<uInt>(data.size());

  zstream.next_out = reinterpret_cast<unsigned char*>(buf);
  zstream.avail_out = static_cast<decltype(zstream.avail_out)>(availableCapacity);

  const auto rc = deflate(&zstream, Z_FINISH);
  std::size_t written = availableCapacity - zstream.avail_out;
  if (rc != Z_STREAM_END) [[unlikely]] {
    written = 0;
  } else {
    assert(zstream.avail_in == 0);
  }

  return written;
}

}  // namespace aeronet
