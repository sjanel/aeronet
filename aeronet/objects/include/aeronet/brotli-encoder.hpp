#pragma once

#include <brotli/encode.h>

#include <cstddef>
#include <memory>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/object-array-pool.hpp"

namespace aeronet {

/// Brotli memory allocator wrapper using ObjectArrayPool for efficient reuse.
///
/// Memory management strategy:
/// - Allocations are pooled in blocks with exponential growth (2x multiplier)
/// - At the start of each compression session, clear() clears the pool state (but keeps blocks)
/// - This allows efficient reuse of allocated blocks across multiple sessions
/// - Actual memory is freed only when the encoder is destroyed
/// - This design is ideal for long-running servers: allocate once, reuse many times
struct BrotliScratch {
  void clear() noexcept { _pool.clear(); }

  void* alloc(std::size_t size) { return _pool.allocateAndDefaultConstruct(size); }

  static void* Alloc(void* opaque, size_t size);
  static void Free([[maybe_unused]] void* opaque, [[maybe_unused]] void* address) {
    // Note: We don't free individual allocations here because:
    // 1. ObjectArrayPool manages memory in blocks, not individual allocations
    // 2. The pool is cleared at the start of each session (clear() calls _pool.clear())
    //    which destroys objects but keeps blocks allocated for efficient reuse
    // 3. This design allows long-running servers to reuse pool blocks across sessions
    //    without the overhead of repeated allocation/deallocation
    // 4. Actual memory is freed only when the pool is destroyed (end of encoder lifetime)
  }

 private:
  ObjectArrayPool<std::byte> _pool;
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

  [[nodiscard]] std::size_t maxCompressedBytes(std::size_t uncompressedSize) const override;

  [[nodiscard]] std::size_t endChunkSize() const override { return 128UL; }

  int64_t encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) override;

  int64_t end(std::size_t availableCapacity, char* buf) noexcept override;

  /// Initialize (or reinitialize) the compression context with given parameters.
  /// Since Brotli has no public reset API, a new state is created each time.
  void init(int quality, int window);

 private:
  friend class BrotliEncoder;

  struct BrotliStateDeleter {
    void operator()(BrotliEncoderState* ptr) const { BrotliEncoderDestroyInstance(ptr); }
  };

  BrotliScratch* _scratch{nullptr};
  std::unique_ptr<BrotliEncoderState, BrotliStateDeleter> _state;
};

class BrotliEncoder {
 public:
  BrotliEncoder() noexcept = default;

  explicit BrotliEncoder(CompressionConfig::Brotli cfg) : _quality(cfg.quality), _window(cfg.window) {
    _ctx._scratch = &_scratch;
  }

  BrotliEncoder(const BrotliEncoder&) = delete;
  BrotliEncoder& operator=(const BrotliEncoder&) = delete;
  BrotliEncoder(BrotliEncoder&& rhs) noexcept;
  BrotliEncoder& operator=(BrotliEncoder&& rhs) noexcept;

  ~BrotliEncoder() = default;

  std::size_t encodeFull(std::string_view data, std::size_t availableCapacity, char* buf) const;

  EncoderContext* makeContext() {
    _ctx.init(_quality, _window);
    return &_ctx;
  }

 private:
  int _quality{};
  int _window{};
  BrotliScratch _scratch;
  BrotliEncoderContext _ctx;
};

}  // namespace aeronet
