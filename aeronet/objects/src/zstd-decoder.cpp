#include "aeronet/zstd-decoder.hpp"

#include <zstd.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "aeronet/decoder.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace {

class ZstdStreamingContext final : public DecoderContext {
 public:
  ZstdStreamingContext() {
    if (!_stream) {
      throw std::runtime_error("ZstdStreamingContext - ZSTD_createDStream failed");
    }
    ZSTD_initDStream(_stream.get());
  }

  bool decompressChunk(std::string_view chunk, bool finalChunk, std::size_t maxDecompressedBytes,
                       std::size_t decoderChunkSize, RawChars &out) override {
    if (finished()) {
      return chunk.empty();
    }
    if (chunk.empty()) {
      return !finalChunk;
    }

    ZSTD_inBuffer in{chunk.data(), chunk.size(), 0};
    while (in.pos < in.size) {
      out.ensureAvailableCapacityExponential(decoderChunkSize);
      ZSTD_outBuffer output{out.data() + out.size(), out.availableCapacity(), 0};
      const std::size_t ret = ZSTD_decompressStream(_stream.get(), &output, &in);
      if (ZSTD_isError(ret) != 0U) {
        log::error("ZstdDecoder::Decompress - ZSTD_decompressStream failed with error {}", ret);
        return false;
      }
      out.addSize(output.pos);
      if (maxDecompressedBytes != 0 && out.size() > maxDecompressedBytes) {
        return false;
      }
      if (ret == 0U) {
        _stream.reset();
        if (in.pos != in.size) {
          return false;
        }
        break;
      }
    }
    return finished() || !finalChunk;
  }

 private:
  [[nodiscard]] bool finished() const { return _stream == nullptr; }

  std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)> _stream{ZSTD_createDStream(), &ZSTD_freeDStream};
};

}  // namespace

bool ZstdDecoder::Decompress(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                             RawChars &out) {
  ZstdDecoder decoder;
  return decoder.decompressFull(input, maxDecompressedBytes, decoderChunkSize, out);
}

bool ZstdDecoder::decompressFull(std::string_view input, std::size_t maxDecompressedBytes, std::size_t decoderChunkSize,
                                 RawChars &out) {
  const auto rSize = ZSTD_getFrameContentSize(input.data(), input.size());
  if (rSize == ZSTD_CONTENTSIZE_ERROR) {
    log::error("ZstdDecoder::Decompress - getFrameContentSize returned ZSTD_CONTENTSIZE_ERROR");
    return false;
  }
  if (rSize != ZSTD_CONTENTSIZE_UNKNOWN) {
    if (rSize > maxDecompressedBytes) {
      return false;
    }

    out.ensureAvailableCapacityExponential(rSize);
    const std::size_t result = ZSTD_decompress(out.data() + out.size(), rSize, input.data(), input.size());

    const auto errc = ZSTD_isError(result);
    if (errc != 0U && result != rSize) {
      log::error("ZstdDecoder::Decompress - ZSTD_isError failed with error {} or incorrect size {} != {}", errc, result,
                 rSize);
      return false;
    }

    out.addSize(rSize);
    return true;
  }

  // Unknown size: grow progressively

  ZstdStreamingContext ctx;

  return ctx.decompressChunk({input.data(), input.size()}, true, maxDecompressedBytes, decoderChunkSize, out);
}

std::unique_ptr<DecoderContext> ZstdDecoder::makeContext() { return std::make_unique<ZstdStreamingContext>(); }

}  // namespace aeronet
