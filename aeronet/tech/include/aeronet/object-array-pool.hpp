#pragma once

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace aeronet {

// Object array pools for fast allocation of frequently used objects.
// Once allocated and constructed, object pointers remain valid along with the pool lifetime.
// All allocated objects are destroyed when the pool is destroyed.
template <class T, class SizeType = std::size_t>
class ObjectArrayPool {
 public:
  using size_type = SizeType;

  static constexpr size_type kDefaultInitialCapacity = 32U;
  static constexpr size_type kGrowthFactor = 2U;

  // Creates an empty ObjectArrayPool with no preallocated capacity, with the specified initial (minimal) capacity for
  // the first block. The growth factor is 2, so the next block capacity will be the double of last block capacity.
  explicit ObjectArrayPool(size_type initialCapacity = kDefaultInitialCapacity) noexcept
      : _totalCapacity(initialCapacity) {}

  // Disable copy operations.
  ObjectArrayPool(const ObjectArrayPool&) = delete;
  ObjectArrayPool& operator=(const ObjectArrayPool&) = delete;

  // Move operations transfer ownership of the pool.
  // No object pointers are invalidated.
  ObjectArrayPool(ObjectArrayPool&& other) noexcept;
  ObjectArrayPool& operator=(ObjectArrayPool&& other) noexcept;

  ~ObjectArrayPool() { reset(); }

  // Allocates and default constructs an array of nbElems objects in the pool with the provided arguments.
  // Returned pointer remains valid until the pool is destroyed.
  // Note: calling allocate with 0 elements is possible, and returns a non-null pointer
  // that MUST NOT be dereferenced.
  // Exception guarantee: allocateAndDefaultConstruct provides the basic exception
  // guarantee. If construction of T throws, the pool remains in a valid state
  // (no live object is added, pool size is unchanged and the slot is returned
  // to the free-list). The pool's capacity may increase if a new block was
  // allocated before the constructor threw; that allocation is not rolled
  // back by this function.
  // The returned pointer can then be wrapped in a std::span<T> with nbElems size if desired.
  [[nodiscard]] T* allocateAndDefaultConstruct(size_type nbElems);

  // Provided that arr is the last object array returned by allocate,
  // shrinks its size to newSize.
  // This is useful when the exact size of the array is not known at allocation time.
  // Objects are destroyed as needed to reduce the size.
  // Preconditions:
  //  - newSize must be less than or equal to the original allocated size.
  //  - the last write operation called on the pool must be allocateAndDefaultConstruct
  //    that returned arr (In particular, it's undefined behavior to call this method after clear or reset).
  // You can call this method with newSize = 0 to free the entire last allocation.
  void shrinkLastAllocated(const T* arr, size_type newSize) noexcept {
    assert(_pCurrentBlock != nullptr && arr + newSize <= _pCurrentBlock->begin() + _pCurrentBlock->size);
    std::destroy(const_cast<T*>(arr + newSize), _pCurrentBlock->begin() + _pCurrentBlock->size);
    _pCurrentBlock->size = static_cast<size_type>(arr + newSize - _pCurrentBlock->begin());
  }

  // Returns the current capacity (number of allocated slots) of the pool.
  [[nodiscard]] size_type capacity() const noexcept {
    return _pCurrentBlock == nullptr ? size_type{0} : _totalCapacity;
  }

  // Clears the pool, destroying all live objects.
  // Capacity remains untouched (all memory blocks are kept).
  void clear() noexcept;

  // Clears the pool and release all allocated blocks.
  // All live objects are destroyed so all pointers previously returned by
  // allocateAndDefaultConstruct become invalid.
  void reset() noexcept;

 private:
  struct Block {
    T* begin() noexcept { return reinterpret_cast<T*>(reinterpret_cast<std::byte*>(this + 1) + kMallocPadding); }

    Block* pPrevBlock;
    Block* pNextBlock;
    size_type size;
    size_type capacity;
  };

  static constexpr size_type kSlotAlign = static_cast<size_type>(std::alignment_of_v<T>);
  static constexpr size_type kMallocPadding = ((kSlotAlign - (sizeof(Block) % kSlotAlign)) % kSlotAlign);

  size_type getNextBlockCapacity(size_type nbElems) const noexcept {
    size_type newBlockCapacity;
    if (_pCurrentBlock == nullptr) {
      newBlockCapacity = _totalCapacity;
    } else {
      newBlockCapacity = _pCurrentBlock->capacity * kGrowthFactor;
    }
    if (newBlockCapacity < nbElems) {
      newBlockCapacity = nbElems;
    }
    return newBlockCapacity;
  }

  void getOrCreateNewBlock(size_type nbElems) {
    if (_pCurrentBlock != nullptr) {
      Block* pNextBlock = _pCurrentBlock->pNextBlock;
      while (pNextBlock != nullptr) {
        assert(pNextBlock->size == 0);  // must be empty
        _pCurrentBlock = pNextBlock;
        if (nbElems <= pNextBlock->capacity) {
          return;
        }
        pNextBlock = pNextBlock->pNextBlock;
      }
    }

    const size_type newBlockCapa = getNextBlockCapacity(nbElems);

    // We need to add padding to make sure that the Slot array that follows the Block header is properly aligned.
    // malloc itself returns memory aligned to max_align_t, which is sufficient for our needs, but the Block header may
    // have a size that is not a multiple of Slot alignment.
    Block* pNewBlock = static_cast<Block*>(std::malloc(sizeof(Block) + kMallocPadding + (newBlockCapa * sizeof(T))));
    if (pNewBlock == nullptr) {
      throw std::bad_alloc();
    }

    pNewBlock->pPrevBlock = _pCurrentBlock;
    pNewBlock->pNextBlock = nullptr;
    pNewBlock->size = 0;
    pNewBlock->capacity = newBlockCapa;

    if (_pCurrentBlock != nullptr) {
      _pCurrentBlock->pNextBlock = pNewBlock;
      _totalCapacity += newBlockCapa;
    } else {
      _pFirstBlock = pNewBlock;
      _totalCapacity = newBlockCapa;
    }

    // allocation cursor moves to the newly appended block
    _pCurrentBlock = pNewBlock;
  }

  Block* _pFirstBlock{nullptr};
  Block* _pCurrentBlock{nullptr};
  // totalCapacity tracks the total number of allocated slots in the pool
  // or the initial capacity if no blocks have been allocated yet.
  size_type _totalCapacity{kDefaultInitialCapacity};
};

template <class T, class SizeType>
ObjectArrayPool<T, SizeType>::ObjectArrayPool(ObjectArrayPool&& other) noexcept
    : _pFirstBlock(std::exchange(other._pFirstBlock, nullptr)),
      _pCurrentBlock(std::exchange(other._pCurrentBlock, nullptr)),
      _totalCapacity(std::exchange(other._totalCapacity, kDefaultInitialCapacity)) {}

template <class T, class SizeType>
ObjectArrayPool<T, SizeType>& ObjectArrayPool<T, SizeType>::operator=(ObjectArrayPool&& other) noexcept {
  if (this != &other) [[likely]] {
    reset();

    _pFirstBlock = std::exchange(other._pFirstBlock, nullptr);
    _pCurrentBlock = std::exchange(other._pCurrentBlock, nullptr);
    _totalCapacity = std::exchange(other._totalCapacity, kDefaultInitialCapacity);
  }
  return *this;
}

template <class T, class SizeType>
T* ObjectArrayPool<T, SizeType>::allocateAndDefaultConstruct(size_type nbElems) {
  if (_pCurrentBlock == nullptr || _pCurrentBlock->size + nbElems > _pCurrentBlock->capacity) {
    getOrCreateNewBlock(nbElems);
  }

  T* pSlot = _pCurrentBlock->begin() + _pCurrentBlock->size;

  std::uninitialized_default_construct_n(pSlot, nbElems);

  _pCurrentBlock->size += nbElems;

  return pSlot;
}

template <class T, class SizeType>
void ObjectArrayPool<T, SizeType>::clear() noexcept {
  // Destroy constructed objects but keep allocated blocks for reuse.
  // After clear, allocation should start from the first block again.
  for (Block* pBlock = _pFirstBlock; pBlock != nullptr; pBlock = pBlock->pNextBlock) {
    std::destroy_n(pBlock->begin(), pBlock->size);
    pBlock->size = 0;
  }

  // Reset allocation cursor to the beginning
  _pCurrentBlock = _pFirstBlock;
}

template <class T, class SizeType>
void ObjectArrayPool<T, SizeType>::reset() noexcept {
  if (_pFirstBlock != nullptr) {
    clear();

    // Resets total capacity to its initial value (at construction of the pool)
    _totalCapacity = _pFirstBlock->capacity;

    Block* pBlock = _pFirstBlock;
    while (pBlock != nullptr) {
      Block* pNext = pBlock->pNextBlock;
      std::free(pBlock);
      pBlock = pNext;
    }

    _pFirstBlock = nullptr;
    _pCurrentBlock = nullptr;
  }
}

}  // namespace aeronet