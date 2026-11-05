#include "aeronet/zlib-encoder.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/encoder-result.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#include "aeronet/zlib-variant.hpp"

namespace aeronet {

int ZlibEncoderContext::deflateSlices(std::string_view& data, int flush, std::size_t availableCapacity, char* buf,
                                      std::size_t maxSliceSize) {
  assert(maxSliceSize != 0 && maxSliceSize <= kZMaxSize);
  auto& stream = _zs.stream;
  const char* const pBufEnd = buf + availableCapacity;
  char* pOut = buf;
  while (true) {
    const std::size_t sliceSize = std::min(data.size(), maxSliceSize);
    const bool lastSlice = sliceSize == data.size();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    ZSetInput(stream, data.data(), static_cast<uint32_t>(sliceSize));
    ZSetOutput(stream, pOut, ZClampSize(static_cast<std::size_t>(pBufEnd - pOut)));

    const int ret = ZDeflate(stream, lastSlice ? flush : Z_NO_FLUSH);
    data.remove_prefix(sliceSize - stream.avail_in);
    if (lastSlice || ret != Z_OK || stream.avail_in != 0) {
      return ret;
    }
    pOut = ZOutputEnd(stream);
  }
}

EncoderResult ZlibEncoderContext::encodeChunk(std::string_view& data, std::size_t availableCapacity, char* buf) {
  assert(!data.empty());

  // Leaves the input not consumed in data when the output is full (never with minEncodeChunkCapacity() bytes of room).
  if (deflateSlices(data, Z_NO_FLUSH, availableCapacity, buf) == Z_STREAM_ERROR) [[unlikely]] {
    return EncoderResult(EncoderResult::Error::CompressionError);
  }

  return EncoderResult(static_cast<std::size_t>(ZOutputEnd(_zs.stream) - buf));
}

std::size_t ZlibEncoderContext::minEncodeChunkCapacity(std::size_t chunkSize) const {
  auto* pStream = const_cast<zstream*>(&_zs.stream);
  if (chunkSize <= kZMaxDeflateInputSize) [[likely]] {
    return ZDeflateBound(pStream, chunkSize);
  }
  // encodeChunk() deflates a larger chunk in slices.
  return ((chunkSize / kZMaxDeflateInputSize) * ZDeflateBound(pStream, kZMaxDeflateInputSize)) +
         ZDeflateBound(pStream, chunkSize % kZMaxDeflateInputSize);
}

EncoderResult ZlibEncoderContext::end(std::size_t availableCapacity, char* buf) noexcept {
  if (!_zs.initialized()) {
    // The previous call finished the stream.
    return EncoderResult(0);
  }

  // A clamped output room only delays the remaining output to the next end() call.
  const uint32_t room = ZClampSize(availableCapacity);
  ZSetInput(_zs.stream, nullptr, 0);
  ZSetOutput(_zs.stream, buf, room);

  const int ret = ZDeflate(_zs.stream, Z_FINISH);
  if (ret == Z_STREAM_ERROR) {
    return EncoderResult(EncoderResult::Error::CompressionError);
  }

  const std::size_t writtenNow = room - _zs.stream.avail_out;
  if (ret == Z_STREAM_END) {
    // Released now: deflate() refuses a call without output room, even on a finished stream, and the next end() call
    // must report the end of the stream whatever its room.
    _zs.end();
    return EncoderResult(writtenNow);
  }

  if (writtenNow == 0) {
    // Stream not finished and no output room left for its remaining bytes (Z_BUF_ERROR).
    return EncoderResult(EncoderResult::Error::NotEnoughCapacity);
  }

  return EncoderResult(writtenNow);
}

EncoderResult ZlibEncoder::encodeFull(ZlibVariant variant, std::string_view data, std::size_t availableCapacity,
                                      char* buf) {
  _ctx.init(_level, variant);

  const auto rc = _ctx.deflateSlices(data, Z_FINISH, availableCapacity, buf);
  assert(rc != Z_STREAM_END || data.empty());
  if (rc != Z_STREAM_END) {
    // Z_OK: the output is full. Z_BUF_ERROR: no output room is left for the next input slice.
    if (rc == Z_OK || rc == Z_BUF_ERROR) {
      return EncoderResult(EncoderResult::Error::NotEnoughCapacity);
    }
    return EncoderResult(EncoderResult::Error::CompressionError);
  }

  return EncoderResult(static_cast<std::size_t>(ZOutputEnd(_ctx._zs.stream) - buf));
}

}  // namespace aeronet
