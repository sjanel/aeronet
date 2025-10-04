#include "aeronet/zlib-decoder.hpp"

#include <zlib.h>

#include <cstddef>
#include <string_view>

#include "log.hpp"
#include "raw-chars.hpp"

namespace aeronet {

struct ZstreamInflateRAII {
  ZstreamInflateRAII() noexcept = default;

  ZstreamInflateRAII(const ZstreamInflateRAII &) = delete;
  ZstreamInflateRAII(ZstreamInflateRAII &&) noexcept = delete;
  ZstreamInflateRAII &operator=(const ZstreamInflateRAII &) = delete;
  ZstreamInflateRAII &operator=(ZstreamInflateRAII &&) noexcept = delete;

  ~ZstreamInflateRAII() { inflateEnd(&_strm); }

  z_stream _strm{};
};

bool ZlibDecoder::Decompress(std::string_view input, RawChars &out, bool isGzip, std::size_t maxDecompressedBytes) {
  ZstreamInflateRAII strm;

  // windowBits: 16 + MAX_WBITS enables gzip decoding; MAX_WBITS alone for zlib; -MAX_WBITS raw deflate.
  const int windowBits = isGzip ? (16 + MAX_WBITS) : MAX_WBITS;
  const auto ec = inflateInit2(&strm._strm, windowBits);
  if (ec != Z_OK) {
    log::error("ZlibDecoder::Decompress - inflateInit2 failed with error {}", ec);
    return false;
  }

  strm._strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
  strm._strm.avail_in = static_cast<uInt>(input.size());

  // We don’t know decompressed size beforehand → grow dynamically
  // TODO: make this chunk size configurable
  static constexpr size_t kChunkSize = 1 << 14;

  strm._strm.avail_out = static_cast<uInt>(kChunkSize);

  while (true) {
    out.ensureAvailableCapacity(kChunkSize);
    strm._strm.next_out = reinterpret_cast<unsigned char *>(out.data() + out.size());

    const auto ret = inflate(&strm._strm, 0);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      log::error("ZlibDecoder::Decompress - inflate failed with error {}", ret);
      return false;
    }
    out.setSize(out.size() + kChunkSize - strm._strm.avail_out);
    if (ret == Z_STREAM_END) {
      break;
    }

    if (strm._strm.total_out >= maxDecompressedBytes) {
      return false;  // size limit
    }
  }

  return true;
}

}  // namespace aeronet
