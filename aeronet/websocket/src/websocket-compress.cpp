#include "aeronet/websocket-compress.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include "aeronet/decoder-buffer-manager.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"

namespace aeronet {

namespace {
// The 4 trailing bytes (0x00 0x00 0xff 0xff) are removed per RFC 7692 §7.2.1
constexpr std::array kDeflateTrailer{
    std::byte{0x00},
    std::byte{0x00},
    std::byte{0xFF},
    std::byte{0xFF},
};
// Chunk size for streaming decompression with size limit control
constexpr std::size_t kDecompressChunkSize = 16UL * 1024;  // 16 KB
// Output space reserved beyond deflateBound(): it bounds a Z_FINISH deflate, not the sync flush marker. The zlib manual
// asks for more than 6 bytes of avail_out with Z_SYNC_FLUSH: a deflate call returning with avail_out == 0 emits a new
// flush marker at the next call - an endless loop for an empty message, whose raw DEFLATE bound (5 bytes) is exactly
// the size of its marker.
constexpr std::size_t kSyncFlushMargin = 8;
}  // namespace

// ============================================================================
// WebSocketCompressor
// ============================================================================

const char* WebSocketCompressor::compress(std::span<const std::byte> input, RawBytes& output, bool resetContext) {
  auto& stream = _zs.stream;

  if (!_zs.initialized()) {
    // permessage-deflate payloads are raw DEFLATE data (RFC 7692 section 7.2.1), without zlib header nor checksum.
    _zs.initCompress(ZStreamRAII::Variant::raw, _level, _windowBits);
  } else if (resetContext) {
    ZDeflateReset(stream);
  }

  ZSetInput(stream, std::string_view{reinterpret_cast<const char*>(input.data()), input.size()});

  const std::size_t startSize = output.size();
  const auto chunkCapacity = ZDeflateBound(&stream, input.size()) + kSyncFlushMargin;
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

  if (!_zs.initialized()) {
    _zs.initDecompress(ZStreamRAII::Variant::raw, _windowBits);
  } else if (resetContext) {
    ZInflateReset(stream);
  }

  ZSetInput(stream, std::string_view{reinterpret_cast<const char*>(input.data()), input.size()});

  // One byte decompressed beyond the limit tells whether the message exceeds it (0: unlimited).
  const std::size_t initialSize = output.size();
  const bool limited = maxDecompressedSize != 0 && maxDecompressedSize != std::numeric_limits<std::size_t>::max();
  DecoderBufferManager bufferManager(output, kDecompressChunkSize, limited ? maxDecompressedSize + 1 : 0);

  // The message is followed by the sync flush marker removed by the sender (0x00 0x00 0xff 0xff, RFC 7692 section
  // 7.2.2), inflated once the message data is: it leaves the stream at a block boundary, where the next message
  // continues it with context takeover.
  bool trailerSet = false;
  for (;;) {
    // Reserves the next chunk, up to one byte beyond the limit: the decompressed size is checked below.
    bufferManager.nextReserve();

    ZSetOutput(stream, reinterpret_cast<char*>(output.data() + output.size()), output.availableCapacity());

    const auto ret = ZInflate(stream, Z_NO_FLUSH);
    if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
      return "inflate() failed";
    }

    output.addSize(output.availableCapacity() - stream.avail_out);

    if (limited && output.size() - initialSize > maxDecompressedSize) {
      return "Decompressed size exceeds maximum";
    }
    if (stream.avail_out == 0) {
      continue;  // the output is full: inflate may have more of it
    }
    // All the input was inflated (or the sender ended the stream with a final block).
    if (trailerSet || ret == Z_STREAM_END) {
      break;
    }
    ZSetInput(stream, std::string_view{reinterpret_cast<const char*>(kDeflateTrailer.data()), kDeflateTrailer.size()});
    trailerSet = true;
  }

  return nullptr;
}

}  // namespace aeronet
