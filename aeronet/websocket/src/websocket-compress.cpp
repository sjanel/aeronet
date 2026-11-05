#include "aeronet/websocket-compress.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "aeronet/decoder-buffer-manager.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/zlib-gateway.hpp"
#include "aeronet/zlib-stream-raii.hpp"
#include "aeronet/zlib-variant.hpp"

namespace aeronet {

namespace {

// The 4 trailing bytes (0x00 0x00 0xff 0xff) are removed per RFC 7692 §7.2.1
constexpr std::byte kDeflateTrailer[]{
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

// Bit of zstream::data_type set by inflate() when it stops at a block boundary, before the header of the next block.
constexpr unsigned kInflateAtBlockBoundary = 128U;

// Restarts an inflate stream ended by a final block, which inflates nothing more, keeping its sliding window for the
// next messages that refer to it (context takeover, RFC 7692 §7.2.3.4).
void RestartKeepingWindow(zstream& stream) {
  unsigned char window[1U << ZStreamRAII::kMaxWindowBits];
  uint32_t windowSize = 0;
  [[maybe_unused]] auto ret = ZInflateGetDictionary(stream, window, windowSize);
  assert(ret == Z_OK);
  ret = ZInflateReset(stream);
  assert(ret == Z_OK);
  if (windowSize != 0) {
    ret = ZInflateSetDictionary(stream, window, windowSize);
    assert(ret == Z_OK);
  }
}

}  // namespace

// ============================================================================
// WebSocketCompressor
// ============================================================================

const char* WebSocketCompressor::compress(std::span<const std::byte> input, RawBytes& output, bool resetContext,
                                          std::size_t maxSliceSize) {
  assert(maxSliceSize != 0 && maxSliceSize <= kZMaxDeflateInputSize);
  auto& stream = _zs.stream;

  if (!_zs.initialized()) {
    // permessage-deflate payloads are raw DEFLATE data (RFC 7692 section 7.2.1), without zlib header nor checksum.
    _zs.initCompress(ZlibVariant::raw, _level, _windowBits);
  } else if (resetContext) {
    ZDeflateReset(stream);
  }

  const std::size_t startSize = output.size();
  // Only the last input slice is flushed.
  do {
    const auto slice = input.first(std::min(input.size(), maxSliceSize));
    input = input.subspan(slice.size());
    ZSetInput(stream, reinterpret_cast<const char*>(slice.data()), static_cast<uint32_t>(slice.size()));

    const int flush = input.empty() ? Z_SYNC_FLUSH : Z_NO_FLUSH;
    const auto chunkCapacity = ZDeflateBound(&stream, slice.size()) + kSyncFlushMargin;
    do {
      output.ensureAvailableCapacityExponential(chunkCapacity);

      const uint32_t room = ZClampSize(output.availableCapacity());

      ZSetOutput(stream, reinterpret_cast<char*>(output.data() + output.size()), room);

      const auto ret = ZDeflate(stream, flush);
      if (ret == Z_STREAM_ERROR) {
        return "deflate() failed with Z_STREAM_ERROR";
      }

      output.addSize(room - stream.avail_out);

    } while (stream.avail_out == 0);
  } while (!input.empty());

  // Remove the trailing 0x00 0x00 0xff 0xff per RFC 7692 §7.2.1
  const std::size_t compressedSize = output.size() - startSize;
  if (compressedSize >= sizeof(kDeflateTrailer)) [[likely]] {
    const auto* tail = reinterpret_cast<const uint8_t*>(output.data() + output.size() - sizeof(kDeflateTrailer));
    if (std::memcmp(tail, kDeflateTrailer, sizeof(kDeflateTrailer)) == 0) {
      output.setSize(output.size() - sizeof(kDeflateTrailer));
    }
  }

  return nullptr;
}

// ============================================================================
// WebSocketDecompressor
// ============================================================================

const char* WebSocketDecompressor::decompress(std::span<const std::byte> input, RawBytes& output,
                                              std::size_t maxDecompressedSize, bool resetContext,
                                              std::size_t maxSliceSize) {
  assert(maxSliceSize != 0 && maxSliceSize <= kZMaxSize);
  if (input.empty()) {
    // An empty message, which zlib compresses to 0x00 (the header of the empty stored block of the trailer). Without
    // this byte, the trailer alone would leave the stream in the middle of a block header, garbling the next messages
    // with context takeover: the stream is left as it is.
    return nullptr;
  }
  auto& stream = _zs.stream;

  if (!_zs.initialized()) {
    _zs.initDecompress(ZlibVariant::raw, _windowBits);
  } else if (resetContext) {
    ZInflateReset(stream);
  }

  const auto feedNextSlice = [&stream, &input, maxSliceSize] {
    const auto slice = input.first(std::min(input.size(), maxSliceSize));
    ZSetInput(stream, reinterpret_cast<const char*>(slice.data()), static_cast<uint32_t>(slice.size()));
    input = input.subspan(slice.size());
  };
  feedNextSlice();

  DecoderBufferManager bufferManager(output, kDecompressChunkSize, maxDecompressedSize);

  // The message is followed by the sync flush marker removed by the sender (0x00 0x00 0xff 0xff, RFC 7692 section
  // 7.2.2), inflated once the message data is: it leaves the stream at a block boundary, where the next message
  // continues it with context takeover.
  bool trailerSet = false;
  for (;;) {
    // Reserves the next chunk, up to one byte beyond the limit: the decompressed size is checked below.
    const uint32_t room = ZClampSize(bufferManager.nextReserve());

    ZSetOutput(stream, reinterpret_cast<char*>(output.data() + output.size()), room);

    const auto ret = ZInflate(stream, Z_NO_FLUSH);
    if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
      return "inflate() failed";
    }

    output.addSize(room - stream.avail_out);

    if (bufferManager.limitExceeded()) {
      return "Decompressed size exceeds maximum";
    }
    if (ret == Z_STREAM_END) {
      // The sender ended the message with a final block (RFC 7692 §7.2.3.4): nothing of the message may follow it, only
      // a part of the trailer appended to it.
      if (!trailerSet && (stream.avail_in != 0 || !input.empty())) {
        return "Data after the final DEFLATE block";
      }
      if (!resetContext) {
        RestartKeepingWindow(stream);
      }
      break;
    }
    if (stream.avail_out == 0) {
      continue;  // the output is full: inflate may have more of it
    }
    // The input slice was inflated.
    if (trailerSet && input.empty()) {
      // The trailer ends the data at a block boundary, where inflate() waits for the next block header. Elsewhere, the
      // data was truncated: the rest of its last block is missing.
      if ((static_cast<unsigned>(stream.data_type) & kInflateAtBlockBoundary) == 0U) {
        return "Truncated DEFLATE data";
      }
      break;
    }
    if (input.empty()) {
      input = kDeflateTrailer;
      trailerSet = true;
    }
    feedNextSlice();
  }

  return nullptr;
}

}  // namespace aeronet
