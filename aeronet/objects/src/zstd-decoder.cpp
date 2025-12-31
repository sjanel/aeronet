#include "aeronet/zstd-decoder.hpp"

#include <zstd.h>

#include <cstddef>
#include <memory>
#include <new>
#include <string_view>

#include "aeronet/decoder.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "decoder-helpers.hpp"

namespace aeronet {

namespace {

class ZstdStreamingContext final : public DecoderContext {
 public:
  ZstdStreamingContext() {
    if (!_stream) {
      throw std::bad_alloc();
    }
    ZSTD_initDStream(_stream.get());
  }

  bool decompressChunk(std::string_view chunk, [[maybe_unused]] bool finalChunk, std::size_t maxDecompressedBytes,
                       std::size_t decoderChunkSize, RawChars &out) override {
    if (chunk.empty()) {
      return true;
    }
    DecoderBufferManager decoderBufferManager(out, decoderChunkSize, maxDecompressedBytes);

    ZSTD_inBuffer in{chunk.data(), chunk.size(), 0};
    while (in.pos < in.size) {
      const bool forceEnd = decoderBufferManager.nextReserve();
      ZSTD_outBuffer output{out.data() + out.size(), out.availableCapacity(), 0};
      const std::size_t ret = ZSTD_decompressStream(_stream.get(), &output, &in);
      if (ZSTD_isError(ret) != 0U) [[unlikely]] {
        log::error("ZstdDecoder::Decompress - ZSTD_decompressStream failed with error {}", ZSTD_getErrorName(ret));
        return false;
      }
      out.addSize(output.pos);
      if (ret != 0) {
        if (forceEnd) {
          return false;
        }
        continue;
      }
      _stream.reset();
      return in.pos == in.size;
    }
    return true;
  }

 private:
  std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)> _stream{ZSTD_createDStream(), &ZSTD_freeDStream};
};

}  // namespace

bool ZstdDecoder::Decompress(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                             RawChars &out) {
  return ZstdDecoder{}.decompressFull(input, maxDecompressedBytes, decoderChunkSize, out);
}

bool ZstdDecoder::decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                                 RawChars &out) {
  const auto rSize = ZSTD_getFrameContentSize(input.data(), input.size());
  switch (rSize) {
    case ZSTD_CONTENTSIZE_ERROR:
      log::error("ZstdDecoder::Decompress - getFrameContentSize returned ZSTD_CONTENTSIZE_ERROR");
      return false;
    case ZSTD_CONTENTSIZE_UNKNOWN:
      return ZstdStreamingContext{}.decompressChunk(input, true, maxDecompressedBytes, decoderChunkSize, out);
    default: {
      if (maxDecompressedBytes < rSize) {
        return false;
      }

      out.ensureAvailableCapacityExponential(rSize);
      const std::size_t ret = ZSTD_decompress(out.data() + out.size(), rSize, input.data(), input.size());
      if (ZSTD_isError(ret) != 0U) [[unlikely]] {
        log::error("ZstdDecoder::Decompress - ZSTD_decompress failed with error {}", ZSTD_getErrorName(ret));
        return false;
      }

      out.addSize(rSize);
      return true;
    }
  }
}

std::unique_ptr<DecoderContext> ZstdDecoder::makeContext() { return std::make_unique<ZstdStreamingContext>(); }

}  // namespace aeronet
