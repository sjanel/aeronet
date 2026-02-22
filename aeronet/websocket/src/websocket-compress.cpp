#include "aeronet/websocket-compress.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "aeronet/decoder-buffer-manager.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

namespace {
// The 4 trailing bytes (0x00 0x00 0xff 0xff) are removed per RFC 7692 §7.2.1
constexpr std::array<std::byte, 4> kDeflateTrailer = {std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
                                                      std::byte{0xFF}};
// Chunk size for streaming decompression with size limit control
constexpr std::size_t kDecompressChunkSize = 16UL * 1024;  // 16 KB
}  // namespace

// ============================================================================
// WebSocketCompressor
// ============================================================================

WebSocketCompressor::WebSocketCompressor(int8_t compressionLevel)
    : _zs(ZStreamRAII::Variant::deflate, compressionLevel) {}

const char* WebSocketCompressor::compress(std::span<const std::byte> input, RawBytes& output, bool resetContext) {
  auto& stream = _zs.stream;

  if (resetContext) {
    ZDeflateReset(stream);
  }

  ZSetInput(stream, std::string_view{reinterpret_cast<const char*>(input.data()), input.size()});

  const std::size_t startSize = output.size();
  const auto chunkCapacity = ZDeflateBound(&stream, input.size());
  do {
    output.ensureAvailableCapacityExponential(chunkCapacity);

    const auto availableCapacity = output.availableCapacity();

    ZSetOutput(stream, reinterpret_cast<char*>(output.data() + output.size()), availableCapacity);

    const auto ret = ZDeflate(stream, Z_SYNC_FLUSH);
    if (ret == Z_STREAM_ERROR) {
      return "deflate() failed with Z_STREAM_ERROR";
    }

    output.addSize(availableCapacity - stream.avail_out);

  } while (stream.avail_out == 0);

  // Remove the trailing 0x00 0x00 0xff 0xff per RFC 7692 §7.2.1
  const std::size_t compressedSize = output.size() - startSize;
  if (compressedSize >= kDeflateTrailer.size()) [[likely]] {
    const auto* tail = reinterpret_cast<const uint8_t*>(output.data() + output.size() - kDeflateTrailer.size());
    if (std::memcmp(tail, kDeflateTrailer.data(), kDeflateTrailer.size()) == 0) {
      output.setSize(output.size() - kDeflateTrailer.size());
    }
  }

  return nullptr;
}

// ============================================================================
// WebSocketDecompressor
// ============================================================================

const char* WebSocketDecompressor::decompress(std::span<const std::byte> input, RawBytes& output,
                                              std::size_t maxDecompressedSize, bool resetContext) {
  auto& stream = _zs.stream;

  if (resetContext) {
    ZInflateReset(stream);
  }

  ZSetInput(stream, std::string_view{reinterpret_cast<const char*>(input.data()), input.size()});

  // Use DecoderBufferManager to properly control max decompressed size
  DecoderBufferManager bufferManager(output, kDecompressChunkSize, maxDecompressedSize);

  auto flush = Z_NO_FLUSH;
  do {
    bool forceEnd = bufferManager.nextReserve();

    ZSetOutput(stream, reinterpret_cast<char*>(output.data() + output.size()), output.availableCapacity());

    const auto ret = ZInflate(stream, flush);
    if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
      return "inflate() failed";
    }

    output.addSize(output.availableCapacity() - stream.avail_out);

    if (forceEnd) {
      return "Decompressed size exceeds maximum";
    }

    // We also need to append the trailing 0x00 0x00 0xff 0xff that was stripped per RFC 7692 §7.2.1
    if (stream.avail_in == 0 && flush == Z_NO_FLUSH) {
      ZSetInput(stream,
                std::string_view{reinterpret_cast<const char*>(kDeflateTrailer.data()), kDeflateTrailer.size()});

      flush = Z_FINISH;
    }
  } while (stream.avail_out == 0);

  return nullptr;
}

}  // namespace aeronet
