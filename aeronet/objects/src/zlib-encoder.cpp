#include "aeronet/zlib-encoder.hpp"

#include <zconf.h>
#include <zlib.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string_view>

#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

ZlibEncoderContext::ZlibEncoderContext(ZStreamRAII::Variant variant, RawChars& sharedBuf, int8_t level)
    : _buf(sharedBuf), _zs(variant, level) {}

std::string_view ZlibEncoderContext::encodeChunk(std::string_view chunk) {
  _buf.clear();

  _zs.stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(chunk.data()));
  _zs.stream.avail_in = static_cast<uInt>(chunk.size());

  const auto flush = chunk.empty() ? Z_FINISH : Z_NO_FLUSH;
  const auto chunkCapacity = deflateBound(&_zs.stream, static_cast<uLong>(chunk.size()));
  do {
    _buf.ensureAvailableCapacityExponential(chunkCapacity);

    const auto availableCapacity = _buf.availableCapacity();

    _zs.stream.next_out = reinterpret_cast<unsigned char*>(_buf.data() + _buf.size());
    _zs.stream.avail_out = static_cast<decltype(_zs.stream.avail_out)>(availableCapacity);

    const auto ret = deflate(&_zs.stream, flush);
    if (ret == Z_STREAM_ERROR) [[unlikely]] {
      throw std::runtime_error(std::format("Zlib streaming error {}", ret));
    }

    _buf.addSize(availableCapacity - _zs.stream.avail_out);

    if (ret == Z_STREAM_END) {
      break;
    }
  } while (_zs.stream.avail_out == 0);

  assert(_zs.stream.avail_in == 0);

  return _buf;
}

std::size_t ZlibEncoder::encodeFull(std::string_view data, std::size_t availableCapacity, char* buf) {
  ZStreamRAII zs(_variant, _level);

  auto& zstream = zs.stream;

  zstream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  zstream.avail_in = static_cast<uInt>(data.size());

  zstream.next_out = reinterpret_cast<unsigned char*>(buf);
  zstream.avail_out = static_cast<decltype(zstream.avail_out)>(availableCapacity);

  const auto rc = deflate(&zstream, Z_FINISH);
  std::size_t written = availableCapacity - zstream.avail_out;
  if (rc != Z_STREAM_END) {
    written = 0;
  } else {
    assert(zstream.avail_in == 0);
  }

  return written;
}

}  // namespace aeronet
