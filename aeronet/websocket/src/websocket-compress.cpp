#include "aeronet/websocket-compress.hpp"

#include <zconf.h>
#include <zlib.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

namespace {
// The 4 trailing bytes (0x00 0x00 0xff 0xff) are removed per RFC 7692 §7.2.1
constexpr std::array<std::byte, 4> kDeflateTrailer = {std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
                                                      std::byte{0xFF}};
}  // namespace

// ============================================================================
// WebSocketCompressor
// ============================================================================

WebSocketCompressor::WebSocketCompressor(int8_t compressionLevel)
    : _zs(ZStreamRAII::Variant::deflate, compressionLevel) {}

const char* WebSocketCompressor::compress(std::span<const std::byte> input, RawBytes& output, bool resetContext) {
  auto& deflateStream = _zs.stream;

  if (resetContext) {
    deflateReset(&deflateStream);
  }

  deflateStream.avail_in = static_cast<uInt>(input.size());
  deflateStream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));

  const std::size_t startSize = output.size();

  do {
    output.ensureAvailableCapacityExponential(1UL << 16);

    const auto availableCapacity = output.availableCapacity();

    deflateStream.avail_out = static_cast<uInt>(availableCapacity);
    deflateStream.next_out = reinterpret_cast<Bytef*>(output.data() + output.size());

    const auto ret = deflate(&deflateStream, Z_SYNC_FLUSH);
    if (ret == Z_STREAM_ERROR) {
      return "deflate() failed with Z_STREAM_ERROR";
    }

    output.addSize(availableCapacity - deflateStream.avail_out);

  } while (deflateStream.avail_out == 0);

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
  auto& inflateStream = _zs.stream;

  if (resetContext) {
    inflateReset(&inflateStream);
  }

  // We need to append the trailing 0x00 0x00 0xff 0xff that was stripped per RFC 7692
  RawChars inputWithTrailer(input.size() + kDeflateTrailer.size());
  inputWithTrailer.unchecked_append(reinterpret_cast<const char*>(input.data()), input.size());
  inputWithTrailer.unchecked_append(reinterpret_cast<const char*>(kDeflateTrailer.data()), kDeflateTrailer.size());

  inflateStream.avail_in = static_cast<uInt>(inputWithTrailer.size());
  inflateStream.next_in = reinterpret_cast<Bytef*>(inputWithTrailer.data());

  const std::size_t startSize = output.size();

  do {
    output.ensureAvailableCapacityExponential(1UL << 16);

    const auto availableCapacity = output.availableCapacity();

    inflateStream.avail_out = static_cast<uInt>(availableCapacity);
    inflateStream.next_out = reinterpret_cast<Bytef*>(output.data() + output.size());

    const auto ret = inflate(&inflateStream, Z_SYNC_FLUSH);
    if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
      return "inflate() failed";
    }

    output.addSize(availableCapacity - inflateStream.avail_out);

    // Check size limit
    if (maxDecompressedSize > 0 && (output.size() - startSize) > maxDecompressedSize) {
      return "Decompressed size exceeds maximum";
    }
  } while (inflateStream.avail_out == 0);

  return nullptr;
}

}  // namespace aeronet
