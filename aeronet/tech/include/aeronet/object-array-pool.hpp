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

  // Creates an empty ObjectArrayPool with no preallocated capacity.
  // At the first allocation, a block of default initial capacity will be allocated.
  ObjectArrayPool() noexcept = default;

  // Custom constructor with initial capacity.
  // The growth factor is 2, so the next block capacity will be the double of last block capacity.
  explicit ObjectArrayPool(size_type initialCapacity) : _totalCapacity(initialCapacity) {
    getOrCreateNewBlock(initialCapacity);
  }

  // Disable copy operations.
  ObjectArrayPool(const ObjectArrayPool &) = delete;
  ObjectArrayPool &operator=(const ObjectArrayPool &) = delete;

  // Move operations transfer ownership of the pool.
  // No object pointers are invalidated.
  ObjectArrayPool(ObjectArrayPool &&other) noexcept;
  ObjectArrayPool &operator=(ObjectArrayPool &&other) noexcept;

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
  [[nodiscard]] T *allocateAndDefaultConstruct(size_type nbElems);

  // Provided that arr is the last object array returned by allocate,
  // shrinks its size to newSize.
  // This is useful when the exact size of the array is not known at allocation time.
  // Objects are destroyed as needed to reduce the size.
  // Preconditions:
  //  - newSize must be less than or equal to the original allocated size.
  //  - the last write operation called on the pool must be allocateAndDefaultConstruct
  //    that returned arr (In particular, it's undefined behavior to call this method after clear or reset).
  // You can call this method with newSize = 0 to free the entire last allocation.
  void shrinkLastAllocated(T *arr, size_type newSize) noexcept {
    assert(_currentBlock != nullptr && arr + newSize <= _currentBlock->begin() + _currentBlock->size);
    std::destroy(arr + newSize, _currentBlock->begin() + _currentBlock->size);
    _currentBlock->size = static_cast<size_type>(arr + newSize - _currentBlock->begin());
  }

  // Returns the current capacity (number of allocated slots) of the pool.
  [[nodiscard]] size_type capacity() const noexcept { return _currentBlock == nullptr ? size_type{0} : _totalCapacity; }

  // Clears the pool, destroying all live objects.
  // Capacity remains untouched (all memory blocks are kept).
  void clear() noexcept;

  // Clears the pool and release all allocated blocks.
  // All live objects are destroyed so all pointers previously returned by
  // allocateAndDefaultConstruct become invalid.
  void reset() noexcept;

 private:
  struct Block {
    T *begin() noexcept { return reinterpret_cast<T *>(reinterpret_cast<std::byte *>(this + 1) + kMallocPadding); }

    Block *prevBlock;
    Block *nextBlock;
    size_type size;
    size_type capacity;
  };

  static constexpr size_type kSlotAlign = static_cast<size_type>(std::alignment_of_v<T>);
  static constexpr size_type kMallocPadding = ((kSlotAlign - (sizeof(Block) % kSlotAlign)) % kSlotAlign);

  size_type getNextBlockCapacity(size_type nbElems) const noexcept {
    size_type newBlockCapacity;
    if (_currentBlock == nullptr) {
      newBlockCapacity = _totalCapacity;
    } else {
      newBlockCapacity = _currentBlock->capacity * kGrowthFactor;
    }
    if (newBlockCapacity < nbElems) {
      newBlockCapacity = nbElems;
    }
    return newBlockCapacity;
  }

  void getOrCreateNewBlock(size_type nbElems) {
    if (_currentBlock != nullptr) {
      Block *nextBlock = _currentBlock->nextBlock;
      while (nextBlock != nullptr) {
        assert(nextBlock->size == 0);  // must be empty
        _currentBlock = nextBlock;
        if (nbElems <= nextBlock->capacity) {
          return;
        }
        nextBlock = nextBlock->nextBlock;
      }
    }

    const size_type newBlockCapa = getNextBlockCapacity(nbElems);

    // We need to add padding to make sure that the Slot array that follows
    // the Block header is properly aligned.
    // malloc itselfs returns memory aligned to max_align_t, which is
    // sufficient for our needs, but the Block header may have a size
    // that is not a multiple of Slot alignment.
    Block *newBlock = static_cast<Block *>(std::malloc(sizeof(Block) + kMallocPadding + (newBlockCapa * sizeof(T))));
    if (newBlock == nullptr) {
      throw std::bad_alloc();
    }

    newBlock->prevBlock = _currentBlock;
    newBlock->nextBlock = nullptr;
    newBlock->size = 0;
    newBlock->capacity = newBlockCapa;

    if (_currentBlock != nullptr) {
      _currentBlock->nextBlock = newBlock;
      _totalCapacity += newBlockCapa;
    } else {
      _firstBlock = newBlock;
    }

    // allocation cursor moves to the newly appended block
    _currentBlock = newBlock;
  }

  Block *_firstBlock{nullptr};
  Block *_currentBlock{nullptr};
  // totalCapacity tracks the total number of allocated slots in the pool
  // or the initial capacity if no blocks have been allocated yet.
  size_type _totalCapacity{kDefaultInitialCapacity};
};

template <class T, class SizeType>
ObjectArrayPool<T, SizeType>::ObjectArrayPool(ObjectArrayPool &&other) noexcept
    : _firstBlock(std::exchange(other._firstBlock, nullptr)),
      _currentBlock(std::exchange(other._currentBlock, nullptr)),
      _totalCapacity(std::exchange(other._totalCapacity, kDefaultInitialCapacity)) {}

template <class T, class SizeType>
ObjectArrayPool<T, SizeType> &ObjectArrayPool<T, SizeType>::operator=(ObjectArrayPool &&other) noexcept {
  if (this != &other) [[likely]] {
    reset();
    _firstBlock = std::exchange(other._firstBlock, nullptr);
    _currentBlock = std::exchange(other._currentBlock, nullptr);
    _totalCapacity = std::exchange(other._totalCapacity, kDefaultInitialCapacity);
  }
  return *this;
}

template <class T, class SizeType>
T *ObjectArrayPool<T, SizeType>::allocateAndDefaultConstruct(size_type nbElems) {
  if (_currentBlock == nullptr || _currentBlock->size + nbElems > _currentBlock->capacity) {
    getOrCreateNewBlock(nbElems);
  }

  T *slot = _currentBlock->begin() + _currentBlock->size;

  std::uninitialized_default_construct_n(slot, nbElems);

  _currentBlock->size += nbElems;

  return slot;
}

template <class T, class SizeType>
void ObjectArrayPool<T, SizeType>::clear() noexcept {
  // Destroy constructed objects but keep allocated blocks for reuse.
  // After clear, allocation should start from the first block again.
  for (Block *block = _firstBlock; block != nullptr; block = block->nextBlock) {
    std::destroy_n(block->begin(), block->size);
    block->size = 0;
  }

  // Reset allocation cursor to the beginning
  _currentBlock = _firstBlock;
}

template <class T, class SizeType>
void ObjectArrayPool<T, SizeType>::reset() noexcept {
  if (_firstBlock != nullptr) {
    clear();

    // Resets total capacity to its initial value (at construction of the pool)
    _totalCapacity = _firstBlock->capacity;

    Block *block = _firstBlock;
    while (block != nullptr) {
      Block *next = block->nextBlock;
      std::free(block);
      block = next;
    }

    _firstBlock = nullptr;
    _currentBlock = nullptr;
  }
}

}  // namespace aeronet