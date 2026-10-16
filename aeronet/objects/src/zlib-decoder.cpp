#include "aeronet/zlib-decoder.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "aeronet/decoder-buffer-manager.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

bool ZlibDecoderContext::decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                                         uint32_t decoderChunkSize, RawChars& out) {
  if (chunk.empty()) {
    return !finalChunk || _streamEnded;
  }

  auto& stream = _zs.stream;

  ZSetInput(stream, chunk);

  DecoderBufferManager decoderBufferManager(out, decoderChunkSize, maxDecompressedBytes);

  while (true) {
    // zlib counts the output room in 32 bits.
    const auto room = static_cast<uint32_t>(
        std::min<std::size_t>(decoderBufferManager.nextReserve(), std::numeric_limits<uint32_t>::max()));

    ZSetOutput(stream, out.data() + out.size(), room);

    const auto ret = ZInflate(stream, Z_NO_FLUSH);
    out.addSize(room - stream.avail_out);
    if (decoderBufferManager.limitExceeded()) {
      log::debug("ZlibDecoderContext::decompressChunk - exceeded max decompressed size of {}", maxDecompressedBytes);
      return false;
    }
    if (ret == Z_STREAM_END) {
      _streamEnded = true;
      return stream.avail_in == 0;
    }
    if (ret == Z_BUF_ERROR) {
      // No progress possible with output room: nothing left to decode or flush without more input. The stream continues
      // in the next chunk, so it is truncated when this one is the last.
      assert(stream.avail_in == 0);
      return !finalChunk;
    }
    if (ret != Z_OK) [[unlikely]] {
      log::debug("ZlibDecoderContext::decompressChunk - inflate failed with error {}", ret);
      return false;
    }
    if (stream.avail_in == 0 && stream.avail_out != 0) {
      // All the input is consumed with output room left (so nothing pending): same as above. A full output may hold
      // back decoded bytes, flushed by the next iteration.
      return !finalChunk;
    }
  }
}

}  // namespace aeronet
