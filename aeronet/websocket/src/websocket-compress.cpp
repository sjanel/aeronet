#include "aeronet/websocket-compress.hpp"

#include <zconf.h>
#include <zlib.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "aeronet/decoder-buffer-manager.hpp"
#include "aeronet/raw-bytes.hpp"
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
    deflateReset(&stream);
  }

  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));

  const std::size_t startSize = output.size();
  const auto chunkCapacity = deflateBound(&stream, static_cast<uLong>(input.size()));
  do {
    output.ensureAvailableCapacityExponential(chunkCapacity);

    const auto availableCapacity = output.availableCapacity();

    stream.avail_out = static_cast<uInt>(availableCapacity);
    stream.next_out = reinterpret_cast<Bytef*>(output.data() + output.size());

    const auto ret = deflate(&stream, Z_SYNC_FLUSH);
    if (ret == Z_STREAM_ERROR) {
      return "deflate() failed with Z_STREAM_ERROR";
    }

    output.addSize(availableCapacity - stream.avail_out);

  } while (stream.avail_out == 0);

  // Remove the trailing 0x00 0x00 0xff 0xff per RFC 7692 §7.2.1
  const std::size_t compressedSize = output.size() - startSize;
  if (compressedSize >= kDeflateTrailer.size()) {
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
    inflateReset(&stream);
  }

  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));

  // Use DecoderBufferManager to properly control max decompressed size
  DecoderBufferManager bufferManager(output, kDecompressChunkSize, maxDecompressedSize);

  auto flush = Z_NO_FLUSH;
  do {
    bool forceEnd = bufferManager.nextReserve();

    stream.avail_out = static_cast<uInt>(output.availableCapacity());
    stream.next_out = reinterpret_cast<Bytef*>(output.data() + output.size());

    const auto ret = inflate(&stream, flush);
    if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
      return "inflate() failed";
    }

    output.addSize(output.availableCapacity() - stream.avail_out);

    if (forceEnd) {
      return "Decompressed size exceeds maximum";
    }

    // We also need to append the trailing 0x00 0x00 0xff 0xff that was stripped per RFC 7692 §7.2.1
    if (stream.avail_in == 0 && flush == Z_NO_FLUSH) {
      stream.avail_in = static_cast<uInt>(kDeflateTrailer.size());
      stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(kDeflateTrailer.data()));
      flush = Z_FINISH;
    }
  } while (stream.avail_out == 0);

  return nullptr;
}

}  // namespace aeronet
