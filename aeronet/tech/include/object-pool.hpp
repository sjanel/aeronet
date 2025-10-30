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

namespace internal {

template <class T, bool isTriviallyDestructible = std::is_trivially_destructible_v<T>>
struct Slot {
  static constexpr std::size_t kStorageSize = sizeof(T *) < sizeof(T) ? sizeof(T) : sizeof(T *);
  static constexpr std::size_t kStorageAlign =
      std::alignment_of_v<T> < std::alignment_of_v<T *> ? std::alignment_of_v<T *> : std::alignment_of_v<T>;

  template <class... Args>
  explicit Slot(Args &&...args) {
    std::construct_at(ptr(), std::forward<Args>(args)...);
    isConstructed = true;
  }

  Slot(const Slot &) = delete;
  Slot(Slot &&) = delete;
  Slot &operator=(const Slot &) = delete;
  Slot &operator=(Slot &&) = delete;

  ~Slot() {
    if (isConstructed) {
      std::destroy_at(ptr());
    }
  }

  void setFree(Slot *next) noexcept {
    if (isConstructed) {
      std::destroy_at(ptr());
      isConstructed = false;
    }
    std::memcpy(&storage, &next, sizeof(Slot *));
  }

  Slot *nextFree() const noexcept {
    assert(!isConstructed);
    Slot *next;
    std::memcpy(&next, &storage, sizeof(Slot *));
    return next;
  }

  T *ptr() noexcept { return std::launder(reinterpret_cast<T *>(storage)); }

  bool isConstructed;
  alignas(kStorageAlign) std::byte storage[kStorageSize];
};

template <class T>
struct Slot<T, true> {
  static constexpr std::size_t kStorageSize = sizeof(T *) < sizeof(T) ? sizeof(T) : sizeof(T *);
  static constexpr std::size_t kStorageAlign =
      std::alignment_of_v<T> < std::alignment_of_v<T *> ? std::alignment_of_v<T *> : std::alignment_of_v<T>;

  template <class... Args>
  explicit Slot(Args &&...args) {
    std::construct_at(ptr(), std::forward<Args>(args)...);
  }

  void setFree(Slot *next) noexcept { std::memcpy(&storage, &next, sizeof(Slot *)); }

  Slot *nextFree() const noexcept {
    Slot *next;
    std::memcpy(&next, &storage, sizeof(Slot *));
    return next;
  }

  T *ptr() noexcept { return std::launder(reinterpret_cast<T *>(storage)); }

  alignas(kStorageAlign) std::byte storage[kStorageSize];
};

}  // namespace internal

// Object pools for fast allocation/deallocation of frequently used objects.
// Once allocated and constructed, object pointers remain valid.
// All allocated objects are destroyed when the pool is destroyed.
template <class T>
class ObjectPool {
 public:
  static constexpr std::size_t kDefaultInitialCapacity = 16U;
  static constexpr std::size_t kGrowthFactor = 2U;

  using size_type = std::size_t;

  // Creates an empty ObjectPool with no preallocated capacity.
  // At the first allocation, a block of default initial capacity will be allocated.
  ObjectPool() noexcept = default;

  // Custom constructor with initial capacity.
  // The initial capacity is rounded up to the next power of two to make the size of previous allocated blocks
  // predictable. The growth factor is 2, so the next block capacity will be the double of last block capacity.
  explicit ObjectPool(size_type initialCapacity) : _totalCapacity(initialCapacity) { addBlock(); }

  // Disable copy operations.
  ObjectPool(const ObjectPool &) = delete;
  ObjectPool &operator=(const ObjectPool &) = delete;

  // Move operations transfer ownership of the pool.
  // No object pointers are invalidated.
  ObjectPool(ObjectPool &&other) noexcept;
  ObjectPool &operator=(ObjectPool &&other) noexcept;

  ~ObjectPool() { reset(); }

  // Allocates and constructs an object in the pool with the provided arguments.
  // Returned pointer remains valid until the pool is destroyed or the object is
  // explicitly destroyed via destroyAndRelease().
  //
  // Exception guarantee: allocateAndConstruct provides the basic exception
  // guarantee. If construction of T throws, the pool remains in a valid state
  // (no live object is added, pool size is unchanged and the slot is returned
  // to the free-list). The pool's capacity may increase if a new block was
  // allocated before the constructor threw; that allocation is not rolled
  // back by this function.
  template <class... Args>
  [[nodiscard]] T *allocateAndConstruct(Args &&...args);

  // Destroys the object and releases its slot in the pool.
  // The given pointer MUST be non-null and MUST be a pointer previously
  // returned by allocateAndConstruct(). Calling this method with a null
  // pointer or calling it more than once for the same object is undefined
  // behavior (the implementation uses an assert to catch null pointers in
  // debug builds).
  void destroyAndRelease(T *obj) noexcept;

  // Releases the object from the pool and returns it.
  // This method is only available when T is MoveConstructible. If T is not
  // move-constructible, calling code will not see this overload.
  // The given pointer MUST be non-null and MUST be a pointer previously
  // returned by allocateAndConstruct().
  [[nodiscard]] T release(T *obj) noexcept
    requires std::is_move_constructible_v<T>;

  // Returns the current capacity (number of allocated slots) of the pool.
  [[nodiscard]] size_type capacity() const noexcept { return _lastBlock == nullptr ? size_type{0} : _totalCapacity; }

  // Returns the number of live (constructed) objects in the pool.
  [[nodiscard]] size_type size() const noexcept { return _liveCount; }

  // Returns true if the pool is empty (no live objects).
  [[nodiscard]] bool empty() const noexcept { return _liveCount == 0U; }

  // Clears the pool, destroying all live objects.
  // Capacity remains allocated for future allocations.
  void clear() noexcept;

  // Clears the pool and release all allocated blocks.
  // All live objects are destroyed so all pointers previously returned by
  // allocateAndConstruct become invalid.
  void reset() noexcept;

 private:
  using Slot = internal::Slot<T, std::is_trivially_destructible_v<T>>;

  struct Block {
    Block *_prevBlock;
    size_type _blockSize;
  };

  static constexpr size_type kSlotAlign = static_cast<size_type>(std::alignment_of_v<Slot>);
  static constexpr size_type kMallocPadding = ((kSlotAlign - (sizeof(Block) % kSlotAlign)) % kSlotAlign);

  static Slot *slotBegin(Block *block) noexcept {
    return reinterpret_cast<Slot *>(reinterpret_cast<std::byte *>(block + 1) + kMallocPadding);
  }
  static Slot *slotEnd(Block *block) noexcept { return slotBegin(block) + block->_blockSize; }

  void addBlock() {
    const size_type newBlockSize = _lastBlock == nullptr ? _totalCapacity : _lastBlock->_blockSize * kGrowthFactor;

    // We need to add padding to make sure that the Slot array that follows
    // the Block header is properly aligned.
    // malloc itselfs returns memory aligned to max_align_t, which is
    // sufficient for our needs, but the Block header may have a size
    // that is not a multiple of Slot alignment.
    Block *newBlock = static_cast<Block *>(std::malloc(sizeof(Block) + kMallocPadding + (newBlockSize * sizeof(Slot))));
    if (newBlock == nullptr) {
      throw std::bad_alloc();
    }

    newBlock->_prevBlock = _lastBlock;
    newBlock->_blockSize = newBlockSize;

    if (_lastBlock != nullptr) {
      _totalCapacity += newBlockSize;
    }
    _lastBlock = newBlock;
    _nextSlot = slotBegin(newBlock);
  }

  [[nodiscard]] static Slot *slotFromObject(T *object) noexcept {
    static_assert(std::is_standard_layout_v<Slot>);

    static constexpr std::size_t kStorageOffset = offsetof(Slot, storage);

    return reinterpret_cast<Slot *>(reinterpret_cast<std::byte *>(object) - kStorageOffset);
  }

  Block *_lastBlock{nullptr};
  Slot *_freeList{nullptr};
  Slot *_nextSlot{nullptr};
  // totalCapacity tracks the total number of allocated slots in the pool
  // or the initial capacity if no blocks have been allocated yet.
  size_type _totalCapacity{kDefaultInitialCapacity};
  size_type _liveCount{0};
};

template <class T>
ObjectPool<T>::ObjectPool(ObjectPool &&other) noexcept
    : _lastBlock(std::exchange(other._lastBlock, nullptr)),
      _freeList(std::exchange(other._freeList, nullptr)),
      _nextSlot(std::exchange(other._nextSlot, nullptr)),
      _totalCapacity(std::exchange(other._totalCapacity, kDefaultInitialCapacity)),
      _liveCount(std::exchange(other._liveCount, 0)) {}

template <class T>
ObjectPool<T> &ObjectPool<T>::operator=(ObjectPool &&other) noexcept {
  if (this != &other) {
    reset();

    _lastBlock = std::exchange(other._lastBlock, nullptr);
    _freeList = std::exchange(other._freeList, nullptr);
    _nextSlot = std::exchange(other._nextSlot, nullptr);
    _totalCapacity = std::exchange(other._totalCapacity, kDefaultInitialCapacity);
    _liveCount = std::exchange(other._liveCount, 0);
  }
  return *this;
}

template <class T>
template <class... Args>
T *ObjectPool<T>::allocateAndConstruct(Args &&...args) {
  Slot *slot;
  if (_freeList == nullptr) {
    if (_lastBlock == nullptr || _nextSlot == slotEnd(_lastBlock)) {
      addBlock();
    }

    slot = _nextSlot;
    ++_nextSlot;
  } else {
    slot = _freeList;
    _freeList = slot->nextFree();
  }

  try {
    ::new (slot) Slot(std::forward<Args>(args)...);
    ++_liveCount;
  } catch (...) {
    slot->setFree(_freeList);
    _freeList = slot;
    throw;
  }

  return slot->ptr();
}

template <class T>
void ObjectPool<T>::destroyAndRelease(T *obj) noexcept {
  assert(obj != nullptr);

  Slot *slot = slotFromObject(obj);

  slot->setFree(_freeList);
  _freeList = slot;
  --_liveCount;
}

template <class T>
T ObjectPool<T>::release(T *obj) noexcept
  requires std::is_move_constructible_v<T>
{
  assert(obj != nullptr);

  Slot *slot = slotFromObject(obj);

  T ret(std::move(*obj));
  slot->setFree(_freeList);
  _freeList = slot;
  --_liveCount;
  return ret;
}

template <class T>
void ObjectPool<T>::clear() noexcept {
  _freeList = nullptr;

  for (Block *block = _lastBlock; block != nullptr; block = block->_prevBlock) {
    auto nbElems = block == _lastBlock ? static_cast<std::size_t>(_nextSlot - slotBegin(block)) : block->_blockSize;
    for (std::size_t pos = 0; pos < nbElems; ++pos) {
      Slot *slot = slotBegin(block) + pos;
      slot->setFree(_freeList);
      _freeList = slot;
    }
  }
  _liveCount = 0U;
}

template <class T>
void ObjectPool<T>::reset() noexcept {
  for (Block *block = _lastBlock; block != nullptr;) {
    Block *prev = block->_prevBlock;
    if constexpr (!std::is_trivially_destructible_v<T>) {
      if (block == _lastBlock) {
        std::destroy(slotBegin(block), _nextSlot);
      } else {
        std::destroy_n(slotBegin(block), block->_blockSize);
      }
    }

    if (prev == nullptr) {
      _totalCapacity = block->_blockSize;
    }

    std::free(block);

    block = prev;
  }

  _lastBlock = nullptr;
  _freeList = nullptr;
  _liveCount = 0U;
  // restore growth state so that subsequent allocations behave as if the pool was
  // constructed with the user provided initial capacity (or default if none).
  _nextSlot = nullptr;
}

}  // namespace aeronet