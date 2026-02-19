#include "aeronet/zlib-decoder.hpp"

#include <cstddef>
#include <string_view>

#include "aeronet/decoder-buffer-manager.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

bool ZlibDecoderContext::decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                                         std::size_t decoderChunkSize, RawChars& out) {
  if (chunk.empty()) {
    return true;
  }

  auto& stream = _zs.stream;

  ZSetInput(stream, chunk);

  DecoderBufferManager decoderBufferManager(out, decoderChunkSize, maxDecompressedBytes);

  while (true) {
    const bool forceEnd = decoderBufferManager.nextReserve();

    ZSetOutput(stream, out.data() + out.size(), out.availableCapacity());

    const auto ret = ZInflate(stream, Z_NO_FLUSH);
    out.setSize(out.capacity() - stream.avail_out);
    if (ret == Z_STREAM_END) {
      return stream.avail_in == 0;
    }
    if (ret != Z_OK) [[unlikely]] {
      log::debug("decompressChunk - inflate failed with error {}", ret);
      return false;
    }
    if (forceEnd) {
      log::debug("decompressChunk - reached max decompressed size of {}", maxDecompressedBytes);
      return false;
    }

    if (stream.avail_in == 0) {
      return !finalChunk;
    }
  }
}

}  // namespace aeronet
