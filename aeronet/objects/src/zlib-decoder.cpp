#include "aeronet/zlib-decoder.hpp"

#include <zconf.h>
#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/decoder.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

namespace {

class ZlibStreamingContext final : public DecoderContext {
 public:
  explicit ZlibStreamingContext(bool isGzip)
      : _context(isGzip ? ZStreamRAII::Variant::gzip : ZStreamRAII::Variant::deflate) {}

  bool decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                       std::size_t decoderChunkSize, RawChars &out) override {
    if (_finished) {
      return chunk.empty();
    }
    if (chunk.empty()) {
      return true;
    }

    auto &stream = _context.stream;

    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(chunk.data()));
    stream.avail_in = static_cast<uInt>(chunk.size());

    const auto initialSize = out.size();

    if (maxDecompressedBytes == 0) {
      maxDecompressedBytes = std::numeric_limits<std::size_t>::max() - initialSize;
    }

    bool forceEnd = false;
    while (true) {
      const auto alreadyDecompressed = out.size() - initialSize;
      std::size_t capacity;
      if (alreadyDecompressed + decoderChunkSize > maxDecompressedBytes) {
        // Reached the maximum allowed decompressed size - force end if current chunk does not reach the end.
        forceEnd = true;
        capacity = initialSize + maxDecompressedBytes;
      } else {
        capacity =
            std::min(std::max(out.size() + decoderChunkSize, out.capacity() * 2UL), initialSize + maxDecompressedBytes);
      }
      out.reserve(capacity);

      stream.avail_out = static_cast<uInt>(out.availableCapacity());
      stream.next_out = reinterpret_cast<unsigned char *>(out.data() + out.size());

      const auto ret = inflate(&stream, Z_NO_FLUSH);
      out.setSize(out.capacity() - stream.avail_out);
      if (ret == Z_STREAM_END) {
        _finished = true;
        return stream.avail_in == 0;
      }
      if (ret != Z_OK) {
        log::error("ZlibDecoder::Decompress - inflate failed with error {}", ret);
        return false;
      }
      if (forceEnd) {
        log::debug("ZlibDecoder::Decompress - reached max decompressed size of {}", maxDecompressedBytes);
        return false;
      }

      if (stream.avail_in == 0) {
        return !finalChunk;
      }
    }
  }

 private:
  ZStreamRAII _context;
  bool _finished{false};
};

}  // namespace

bool ZlibDecoder::Decompress(std::string_view input, bool isGzip, std::size_t maxDecompressedBytes,
                             std::size_t decoderChunkSize, RawChars &out) {
  ZlibStreamingContext context(isGzip);
  return context.decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
}

bool ZlibDecoder::decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                                 RawChars &out) {
  return Decompress(input, _isGzip, maxDecompressedBytes, decoderChunkSize, out);
}

std::unique_ptr<DecoderContext> ZlibDecoder::makeContext() { return std::make_unique<ZlibStreamingContext>(_isGzip); }

}  // namespace aeronet
