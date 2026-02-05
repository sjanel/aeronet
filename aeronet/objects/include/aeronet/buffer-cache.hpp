#pragma once

#include <cstddef>

namespace aeronet::internal {

// BufferCache is a simple utility for caching a single buffer allocation for reuse across multiple operations.
// It tracks ownership of the buffer to allow efficient reuse when possible, while ensuring proper deallocation.
class BufferCache {
 public:
  BufferCache() noexcept = default;

  BufferCache(const BufferCache&) = delete;
  BufferCache(BufferCache&& rhs) noexcept;
  BufferCache& operator=(const BufferCache&) = delete;
  BufferCache& operator=(BufferCache&& rhs) noexcept;

  ~BufferCache();

  // Allocate a buffer of at least the requested size.
  // May return a previously cached buffer if available and large enough.
  void* allocate(std::size_t size) noexcept;

  // Deallocate a buffer previously returned by allocate().
  // The buffer may be cached for reuse if it matches the currently tracked allocation, otherwise it will be freed.
  void deallocate(void* ptr) noexcept;

 private:
  struct BufSize {
    void* pBuf = nullptr;
    std::size_t size = 0;
  };

  BufSize _ownedBuf;
  BufSize _givenBuf;
};

}  // namespace aeronet::internal