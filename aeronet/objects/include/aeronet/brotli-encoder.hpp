#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoder-result.hpp"
#include "aeronet/encoder.hpp"

extern "C" struct BrotliEncoderStateStruct;
using BrotliEncoderState = BrotliEncoderStateStruct;

namespace aeronet {

/// Allocator of a brotli encoder, keeping the blocks brotli frees for its next allocations: brotli allocates the same
/// tables in each session (MBs from quality 5, 32 MiB from quality 9 with the default window), and similar temporaries
/// for each block of input. The cache stays close to the memory brotli uses at once, whatever the length of the
/// streams:
/// - an allocation takes the smallest cached block that fits, unless it is more than twice as large
/// - an allocation finding no such block frees the largest smaller one if it is at least half as large, an outgrown
///   copy of a growing buffer
/// - trim(), between two sessions, frees the cached blocks the last session did not allocate
class BrotliScratch {
 public:
  BrotliScratch() noexcept = default;

  BrotliScratch(const BrotliScratch&) = delete;
  // Moves need brotli to hold no block of rhs: its states keep the address of their scratch.
  BrotliScratch(BrotliScratch&& rhs) noexcept;
  BrotliScratch& operator=(const BrotliScratch&) = delete;
  BrotliScratch& operator=(BrotliScratch&& rhs) noexcept;

  ~BrotliScratch() { release(); }

  /// Frees the cached blocks not allocated since the previous call. Brotli must hold no block: call it between the
  /// destruction of a brotli state and the creation of the next one.
  void trim() noexcept;

  /// Allocation functions given to brotli, with this scratch as opaque pointer.
  static void* Alloc(void* opaque, std::size_t size) noexcept;
  static void Free(void* opaque, void* address) noexcept;

 private:
  friend struct BrotliScratchTestAccess;

  // Header of a block, followed by its data: aligned as max_align_t, so that the data is aligned as malloc memory.
  struct alignas(std::max_align_t) Block {
    // Set in capacityAndAllocated when the block was allocated since the last trim().
    static constexpr std::size_t kAllocatedFlag = 1;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacityAndAllocated & ~kAllocatedFlag; }

    [[nodiscard]] std::byte* data() noexcept { return reinterpret_cast<std::byte*>(this + 1); }

    [[nodiscard]] static Block* FromData(void* address) noexcept {
      return reinterpret_cast<Block*>(static_cast<std::byte*>(address) - sizeof(Block));
    }

    // Capacity of the data in bytes, a multiple of alignof(Block), with kAllocatedFlag in its lowest bit.
    std::size_t capacityAndAllocated;
    Block* pNext;  // next cached block
  };

  void release() noexcept;

  Block* _pFirstCached{nullptr};  // cached blocks, by increasing capacity
};

class BrotliEncoderContext final : public EncoderContext {
 public:
  BrotliEncoderContext() noexcept = default;

  explicit BrotliEncoderContext(BrotliScratch& scratch) : _scratch(&scratch) {}

  BrotliEncoderContext(const BrotliEncoderContext&) = delete;
  BrotliEncoderContext(BrotliEncoderContext&& rhs) noexcept;
  BrotliEncoderContext& operator=(const BrotliEncoderContext&) = delete;
  BrotliEncoderContext& operator=(BrotliEncoderContext&& rhs) noexcept;

  ~BrotliEncoderContext() override = default;

  [[nodiscard]] std::size_t minEncodeChunkCapacity(std::size_t chunkSize) const override;

  [[nodiscard]] std::size_t endChunkSize() const override { return 128UL; }

  EncoderResult encodeChunk(std::string_view& data, std::size_t availableCapacity, char* buf) override;

  EncoderResult end(std::size_t availableCapacity, char* buf) noexcept override;

  /// Initialize (or reinitialize) the compression context with given parameters.
  /// Since Brotli has no public reset API, a new state is created each time, from the blocks its scratch kept.
  void init(int quality, int window);

 private:
  friend class BrotliEncoder;

  struct BrotliStateDeleter {
    void operator()(BrotliEncoderState* ptr) const noexcept;
  };

  BrotliScratch* _scratch{nullptr};
  std::unique_ptr<BrotliEncoderState, BrotliStateDeleter> _state;
};

class BrotliEncoder {
 public:
  BrotliEncoder() noexcept = default;

  explicit BrotliEncoder(CompressionConfig::Brotli cfg) noexcept : _quality(cfg.quality), _window(cfg.window) {
    _ctx._scratch = &_scratch;
  }

  BrotliEncoder(const BrotliEncoder&) = delete;
  BrotliEncoder& operator=(const BrotliEncoder&) = delete;
  BrotliEncoder(BrotliEncoder&& rhs) noexcept;
  BrotliEncoder& operator=(BrotliEncoder&& rhs) noexcept;

  ~BrotliEncoder() = default;

  EncoderResult encodeFull(std::string_view data, std::size_t availableCapacity, char* buf);

  EncoderContext* makeContext() {
    _ctx.init(_quality, _window);
    return &_ctx;
  }

  EncoderContext* context() { return &_ctx; }

 private:
  int _quality{};
  int _window{};
  BrotliScratch _scratch;
  BrotliEncoderContext _ctx;
};

}  // namespace aeronet
