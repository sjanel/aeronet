#include "aeronet/zlib-encoder.hpp"

#include <zconf.h>
#include <zlib.h>

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

std::string_view ZlibEncoderContext::encodeChunk(std::size_t encoderChunkSize, std::string_view chunk) {
  _buf.clear();

  _zs.stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(chunk.data()));
  _zs.stream.avail_in = static_cast<uInt>(chunk.size());

  auto flush = chunk.empty() ? Z_FINISH : Z_NO_FLUSH;
  do {
    _buf.ensureAvailableCapacityExponential(encoderChunkSize);

    const auto availableCapacity = _buf.availableCapacity();

    _zs.stream.next_out = reinterpret_cast<unsigned char*>(_buf.data() + _buf.size());
    _zs.stream.avail_out = static_cast<decltype(_zs.stream.avail_out)>(availableCapacity);

    const auto ret = deflate(&_zs.stream, flush);
    if (ret == Z_STREAM_ERROR) {
      throw std::runtime_error(std::format("Zlib streaming error {}", ret));
    }

    _buf.addSize(availableCapacity - _zs.stream.avail_out);

    if (ret == Z_STREAM_END) {
      break;
    }
  } while (_zs.stream.avail_out == 0 || _zs.stream.avail_in > 0);

  return _buf;
}

void ZlibEncoder::encodeFull(std::size_t extraCapacity, std::string_view data, RawChars& buf) {
  ZStreamRAII zs(_variant, _level);

  auto& zstream = zs.stream;

  zstream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  zstream.avail_in = static_cast<uInt>(data.size());

  std::size_t maxCompressedSize = static_cast<std::size_t>(deflateBound(&zstream, static_cast<uLong>(data.size())));

  buf.ensureAvailableCapacity(maxCompressedSize + extraCapacity);

  std::size_t availableCapacity = buf.availableCapacity();

  zstream.next_out = reinterpret_cast<unsigned char*>(buf.data() + buf.size());
  zstream.avail_out = static_cast<decltype(zstream.avail_out)>(availableCapacity);

  const auto rc = deflate(&zstream, Z_FINISH);
  if (rc != Z_STREAM_END) {
    throw std::runtime_error(
        std::format("Error {} during {} compression", rc, _variant == ZStreamRAII::Variant::gzip ? "gzip" : "deflate"));
  }

  buf.addSize(availableCapacity - zstream.avail_out);
}

}  // namespace aeronet
