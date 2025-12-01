#include "aeronet/zlib-decoder.hpp"

#include <zconf.h>
#include <zlib.h>

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
      return finalChunk ? _finished : true;
    }

    auto &stream = _context.stream;

    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(chunk.data()));
    stream.avail_in = static_cast<uInt>(chunk.size());

    while (true) {
      out.ensureAvailableCapacityExponential(decoderChunkSize);

      stream.avail_out = static_cast<uInt>(out.availableCapacity());
      stream.next_out = reinterpret_cast<unsigned char *>(out.data() + out.size());

      const auto ret = inflate(&stream, Z_NO_FLUSH);
      if (ret != Z_OK && ret != Z_STREAM_END) {
        log::error("ZlibDecoder::Decompress - inflate failed with error {}", ret);
        return false;
      }
      out.setSize(out.capacity() - stream.avail_out);
      if (maxDecompressedBytes != 0 && stream.total_out > maxDecompressedBytes) {
        return false;
      }
      if (ret == Z_STREAM_END) {
        _finished = true;
        return stream.avail_in == 0;
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
  ZlibDecoder decoder(isGzip);
  return decoder.decompressFull(input, maxDecompressedBytes, decoderChunkSize, out);
}

bool ZlibDecoder::decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                                 RawChars &out) {
  ZStreamRAII strm(_isGzip ? ZStreamRAII::Variant::gzip : ZStreamRAII::Variant::deflate);

  auto &zstream = strm.stream;

  zstream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
  zstream.avail_in = static_cast<uInt>(input.size());

  while (true) {
    out.ensureAvailableCapacityExponential(decoderChunkSize);

    zstream.avail_out = static_cast<uInt>(out.availableCapacity());
    zstream.next_out = reinterpret_cast<unsigned char *>(out.data() + out.size());

    const auto ret = inflate(&zstream, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      log::error("ZlibDecoder::Decompress - inflate failed with error {}", ret);
      return false;
    }
    out.setSize(out.capacity() - zstream.avail_out);
    if (ret == Z_STREAM_END) {
      break;
    }

    if (maxDecompressedBytes != 0 && zstream.total_out >= maxDecompressedBytes) {
      return false;  // size limit
    }
  }

  return true;
}

std::unique_ptr<DecoderContext> ZlibDecoder::makeContext() { return std::make_unique<ZlibStreamingContext>(_isGzip); }

}  // namespace aeronet
