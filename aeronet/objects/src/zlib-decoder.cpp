#include "aeronet/zlib-decoder.hpp"

#include <zconf.h>
#include <zlib.h>

#include <cstddef>
#include <string_view>

#include "aeronet/decoder-buffer-manager.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

bool ZlibStreamingContext::decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                                           std::size_t decoderChunkSize, RawChars &out) {
  if (chunk.empty()) {
    return true;
  }

  auto &stream = _context.stream;

  stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(chunk.data()));
  stream.avail_in = static_cast<uInt>(chunk.size());

  DecoderBufferManager decoderBufferManager(out, decoderChunkSize, maxDecompressedBytes);

  while (true) {
    bool forceEnd = decoderBufferManager.nextReserve();

    stream.avail_out = static_cast<uInt>(out.availableCapacity());
    stream.next_out = reinterpret_cast<unsigned char *>(out.data() + out.size());

    const auto ret = inflate(&stream, Z_NO_FLUSH);
    out.setSize(out.capacity() - stream.avail_out);
    if (ret == Z_STREAM_END) {
      return stream.avail_in == 0;
    }
    if (ret != Z_OK) [[unlikely]] {
      log::error("decompressChunk - inflate failed with error {}", ret);
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
