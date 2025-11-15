#include "aeronet/zlib-encoder.hpp"

#include <zconf.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace {
constexpr int ComputeWindowBits(details::ZStreamRAII::Variant variant) {
  switch (variant) {
    case details::ZStreamRAII::Variant::gzip:
      return 15 + 16;
    case details::ZStreamRAII::Variant::deflate:
      return 15;
    default:
      std::unreachable();
  }
}
}  // namespace

namespace details {

ZStreamRAII::ZStreamRAII(Variant variant, int8_t level) {
  const auto ret = deflateInit2(&_stream, level, Z_DEFLATED, ComputeWindowBits(variant), 8, Z_DEFAULT_STRATEGY);
  if (ret != Z_OK) {
    throw std::runtime_error(std::format("Unable to initialize zlib compression - error {}", ret));
  }
}

ZStreamRAII::~ZStreamRAII() {
  if (_stream.state != nullptr) {
    const auto ret = deflateEnd(&_stream);
    if (ret != Z_OK) {
      log::error("zlib: deflateEnd (one-shot) returned {} (ignored)", ret);
    }
  }
}
}  // namespace details

ZlibEncoderContext::ZlibEncoderContext(details::ZStreamRAII::Variant variant, RawChars& sharedBuf, int8_t level)
    : _buf(sharedBuf), _zs(variant, level) {}

std::string_view ZlibEncoderContext::encodeChunk(std::size_t encoderChunkSize, std::string_view chunk) {
  _buf.clear();
  if (_finished) {
    return _buf;
  }
  _zs._stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(chunk.data()));
  _zs._stream.avail_in = static_cast<uInt>(chunk.size());
  auto flush = chunk.empty() ? Z_FINISH : Z_NO_FLUSH;
  do {
    _buf.ensureAvailableCapacityExponential(encoderChunkSize);
    _zs._stream.next_out = reinterpret_cast<unsigned char*>(_buf.data() + _buf.size());
    _zs._stream.avail_out = static_cast<decltype(_zs._stream.avail_out)>(encoderChunkSize);
    const auto ret = deflate(&_zs._stream, flush);
    if (ret == Z_STREAM_ERROR) {
      throw std::runtime_error(std::format("Zlib streaming error {}", ret));
    }
    _buf.addSize(encoderChunkSize - _zs._stream.avail_out);
    if (ret == Z_STREAM_END) {
      _finished = true;
      break;
    }
  } while (_zs._stream.avail_out == 0 || _zs._stream.avail_in > 0);
  if (chunk.empty()) {
    _finished = true;
  }
  return _buf;
}

std::string_view ZlibEncoder::compressAll(std::size_t encoderChunkSize, std::string_view in) {
  details::ZStreamRAII zs(_variant, _level);

  _buf.clear();
  zs._stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs._stream.avail_in = static_cast<uInt>(in.size());
  do {
    _buf.ensureAvailableCapacityExponential(encoderChunkSize);
    zs._stream.next_out = reinterpret_cast<unsigned char*>(_buf.data() + _buf.size());
    zs._stream.avail_out = static_cast<decltype(zs._stream.avail_out)>(encoderChunkSize);
    auto rc = deflate(&zs._stream, Z_FINISH);
    if (rc == Z_STREAM_ERROR) {
      throw std::runtime_error("Zlib error during one-shot compression");
    }
    _buf.addSize(encoderChunkSize - zs._stream.avail_out);
    if (rc == Z_STREAM_END) {
      break;
    }
  } while (zs._stream.avail_out == 0 || zs._stream.avail_in > 0);
  return _buf;
}

}  // namespace aeronet
