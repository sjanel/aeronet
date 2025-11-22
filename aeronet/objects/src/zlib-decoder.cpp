#include "aeronet/zlib-decoder.hpp"

#include <zconf.h>
#include <zlib.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "aeronet/decoder.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace {

struct ZstreamInflateRAII {
  ZstreamInflateRAII() noexcept = default;

  ZstreamInflateRAII(const ZstreamInflateRAII &) = delete;
  ZstreamInflateRAII(ZstreamInflateRAII &&) noexcept = delete;
  ZstreamInflateRAII &operator=(const ZstreamInflateRAII &) = delete;
  ZstreamInflateRAII &operator=(ZstreamInflateRAII &&) noexcept = delete;

  ~ZstreamInflateRAII() { inflateEnd(&_strm); }

  z_stream _strm{};
};

class ZlibStreamingContext final : public DecoderContext {
 public:
  explicit ZlibStreamingContext(bool isGzip) {
    const int windowBits = isGzip ? (16 + MAX_WBITS) : MAX_WBITS;
    if (inflateInit2(&_context._strm, windowBits) != Z_OK) {
      throw std::runtime_error("ZlibStreamingContext - inflateInit2 failed");
    }
  }

  bool decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                       std::size_t decoderChunkSize, RawChars &out) override {
    if (_finished) {
      return chunk.empty();
    }
    if (chunk.empty()) {
      return finalChunk ? _finished : true;
    }

    auto &stream = _context._strm;

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
  ZstreamInflateRAII _context;
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
  ZstreamInflateRAII strm;

  // windowBits: 16 + MAX_WBITS enables gzip decoding; MAX_WBITS alone for zlib; -MAX_WBITS raw deflate.
  const int windowBits = _isGzip ? (16 + MAX_WBITS) : MAX_WBITS;
  const auto ec = inflateInit2(&strm._strm, windowBits);
  if (ec != Z_OK) {
    log::error("ZlibDecoder::Decompress - inflateInit2 failed with error {}", ec);
    return false;
  }

  strm._strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
  strm._strm.avail_in = static_cast<uInt>(input.size());

  while (true) {
    out.ensureAvailableCapacityExponential(decoderChunkSize);
    strm._strm.avail_out = static_cast<uInt>(out.availableCapacity());
    strm._strm.next_out = reinterpret_cast<unsigned char *>(out.data() + out.size());

    const auto ret = inflate(&strm._strm, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      log::error("ZlibDecoder::Decompress - inflate failed with error {}", ret);
      return false;
    }
    out.setSize(out.capacity() - strm._strm.avail_out);
    if (ret == Z_STREAM_END) {
      break;
    }

    if (maxDecompressedBytes != 0 && strm._strm.total_out >= maxDecompressedBytes) {
      return false;  // size limit
    }
  }

  return true;
}

std::unique_ptr<DecoderContext> ZlibDecoder::makeContext() { return std::make_unique<ZlibStreamingContext>(_isGzip); }

}  // namespace aeronet
