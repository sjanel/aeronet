#include "aeronet/zlib-decoder.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/decoder-buffer-manager.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

bool ZlibDecoderContext::decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                                         uint32_t decoderChunkSize, RawChars& out, std::size_t maxSliceSize) {
  assert(maxSliceSize != 0 && maxSliceSize <= kZMaxSize);
  if (chunk.empty()) {
    return !finalChunk || _streamEnded;
  }

  auto& stream = _zs.stream;

  const auto feedNextSlice = [&stream, &chunk, maxSliceSize] {
    const std::string_view slice = chunk.substr(0, maxSliceSize);
    ZSetInput(stream, slice.data(), static_cast<uint32_t>(slice.size()));
    chunk.remove_prefix(slice.size());
  };
  feedNextSlice();

  DecoderBufferManager decoderBufferManager(out, decoderChunkSize, maxDecompressedBytes);

  while (true) {
    const uint32_t room = ZClampSize(decoderBufferManager.nextReserve());

    ZSetOutput(stream, out.data() + out.size(), room);

    const auto ret = ZInflate(stream, Z_NO_FLUSH);
    out.addSize(room - stream.avail_out);
    if (decoderBufferManager.limitExceeded()) {
      log::debug("ZlibDecoderContext::decompressChunk - exceeded max decompressed size of {}", maxDecompressedBytes);
      return false;
    }
    if (ret == Z_STREAM_END) {
      _streamEnded = true;
      return stream.avail_in == 0 && chunk.empty();
    }
    // Z_BUF_ERROR: no progress possible with output room, so nothing left to decode or flush without more input.
    if (ret != Z_OK && ret != Z_BUF_ERROR) [[unlikely]] {
      log::debug("ZlibDecoderContext::decompressChunk - inflate failed with error {}", ret);
      return false;
    }
    assert(ret == Z_OK || stream.avail_in == 0);
    if (stream.avail_in == 0 && stream.avail_out != 0) {
      // The input slice is consumed with output room left (so nothing pending, as always with Z_BUF_ERROR). A full
      // output may hold back decoded bytes, flushed by the next iteration.
      if (chunk.empty()) {
        // The stream continues in the next chunk, so it is truncated when this one is the last.
        return !finalChunk;
      }
      feedNextSlice();
    }
  }
}

}  // namespace aeronet
