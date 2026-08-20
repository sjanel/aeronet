#pragma once

#include <cstddef>
#include <cstdint>
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
  static constexpr std::size_t kStorageSize = sizeof(T*) < sizeof(T) ? sizeof(T) : sizeof(T*);
  static constexpr std::size_t kStorageAlign =
      std::alignment_of_v<T> < std::alignment_of_v<T*> ? std::alignment_of_v<T*> : std::alignment_of_v<T>;

  template <class... Args>
  explicit Slot(Args&&... args) {
    std::construct_at(ptr(), std::forward<Args>(args)...);
    isConstructed = true;
  }

  Slot(const Slot&) = delete;
  Slot(Slot&&) = delete;
  Slot& operator=(const Slot&) = delete;
  Slot& operator=(Slot&&) = delete;

  ~Slot() {
    if (isConstructed) {
      std::destroy_at(ptr());
    }
  }

  void setFree(Slot* next) noexcept {
    if (isConstructed) {
      std::destroy_at(ptr());
      isConstructed = false;
    }
    std::memcpy(&storage, &next, sizeof(Slot*));
  }

  Slot* nextFree() const noexcept {
    Slot* pNext;
    std::memcpy(&pNext, &storage, sizeof(Slot*));
    return pNext;
  }

  T* ptr() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }

  bool isConstructed = false;
  alignas(kStorageAlign) std::byte storage[kStorageSize];
};

template <class T>
struct Slot<T, true> {
  static constexpr std::size_t kStorageSize = sizeof(T*) < sizeof(T) ? sizeof(T) : sizeof(T*);
  static constexpr std::size_t kStorageAlign =
      std::alignment_of_v<T> < std::alignment_of_v<T*> ? std::alignment_of_v<T*> : std::alignment_of_v<T>;

  template <class... Args>
  explicit Slot(Args&&... args) {
    std::construct_at(ptr(), std::forward<Args>(args)...);
  }

  void setFree(Slot* pNext) noexcept { std::memcpy(&storage, &pNext, sizeof(Slot*)); }

  Slot* nextFree() const noexcept {
    Slot* pNext;
    std::memcpy(&pNext, &storage, sizeof(Slot*));
    return pNext;
  }

  T* ptr() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }

  alignas(kStorageAlign) std::byte storage[kStorageSize];
};

}  // namespace internal

template <class T, class SizeType>
class PoolPtr;

// Object pools for fast allocation/deallocation of frequently used objects.
// Once allocated and constructed, object pointers remain valid along with the pool lifetime.
// All allocated objects are destroyed when the pool is destroyed.
template <class T, class SizeType = uint32_t>
class ObjectPool {
 public:
  using size_type = SizeType;

  static constexpr size_type kDefaultInitialCapacity = 16U;
  static constexpr size_type kGrowthFactor = 2U;

  // Creates an empty ObjectPool with no preallocated capacity.
  // The growth factor is 2, so the next block capacity will be the double of last block capacity.
  explicit ObjectPool(size_type initialCapacity = kDefaultInitialCapacity) noexcept : _totalCapacity(initialCapacity) {}

  // Disable copy operations.
  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;

  // Move operations transfer ownership of the pool.
  // No object pointers are invalidated.
  ObjectPool(ObjectPool&& other) noexcept;
  ObjectPool& operator=(ObjectPool&& other) noexcept;

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
  [[nodiscard]] T* allocateAndConstruct(Args&&... args);

  // Destroys the object and releases its slot in the pool.
  // The given pointer MUST be non-null and MUST be a pointer previously
  // returned by allocateAndConstruct(). Calling this method with a null
  // pointer or calling it more than once for the same object is undefined
  // behavior.
  void destroyAndRelease(T* pObj) noexcept;

  // Releases the object from the pool and returns it.
  // This method is only available when T is MoveConstructible. If T is not
  // move-constructible, calling code will not see this overload.
  // The given pointer MUST be non-null and MUST be a pointer previously
  // returned by allocateAndConstruct().
  [[nodiscard]] T release(T* pObj) noexcept
    requires std::is_move_constructible_v<T>;

  // Returns the current capacity (number of allocated slots) of the pool.
  [[nodiscard]] size_type capacity() const noexcept { return _pLastBlock == nullptr ? size_type{0} : _totalCapacity; }

  // Returns the number of live (constructed) objects in the pool.
  [[nodiscard]] size_type size() const noexcept { return _liveCount; }

  // Returns true if the pool is empty (no live objects).
  [[nodiscard]] bool empty() const noexcept { return _liveCount == 0U; }

  // Clears the pool, destroying all live objects.
  // Capacity remains untouched (all memory blocks are kept).
  void clear() noexcept;

  // Clears the pool and release all allocated blocks.
  // All live objects are destroyed so all pointers previously returned by
  // allocateAndConstruct become invalid.
  void reset() noexcept;

  // Allocates and constructs an object in the pool, returning a PoolPtr that manages its lifetime.
  template <class... Args>
  [[nodiscard]] PoolPtr<T, SizeType> allocateAndConstructPoolPtr(Args&&... args) {
    return PoolPtr<T, SizeType>(*this, allocateAndConstruct(std::forward<Args>(args)...));
  }

 private:
  using Slot = internal::Slot<T, std::is_trivially_destructible_v<T>>;

  struct Block {
    Block* _pPrevBlock;
    size_type _blockSize;
  };

  static constexpr size_type kSlotAlign = static_cast<size_type>(std::alignment_of_v<Slot>);
  static constexpr size_type kMallocPadding = ((kSlotAlign - (sizeof(Block) % kSlotAlign)) % kSlotAlign);

  static Slot* slotBegin(Block* pBlock) noexcept {
    return reinterpret_cast<Slot*>(reinterpret_cast<std::byte*>(pBlock + 1) + kMallocPadding);
  }
  static Slot* slotEnd(Block* pBlock) noexcept { return slotBegin(pBlock) + pBlock->_blockSize; }

  void addBlock() {
    const size_type newBlockSize = _pLastBlock == nullptr ? _totalCapacity : _pLastBlock->_blockSize * kGrowthFactor;

    // We need to add padding to make sure that the Slot array that follows
    // the Block header is properly aligned.
    // malloc itselfs returns memory aligned to max_align_t, which is
    // sufficient for our needs, but the Block header may have a size
    // that is not a multiple of Slot alignment.
    Block* pNewBlock = static_cast<Block*>(std::malloc(sizeof(Block) + kMallocPadding + (newBlockSize * sizeof(Slot))));
    if (pNewBlock == nullptr) {
      throw std::bad_alloc();
    }

    pNewBlock->_pPrevBlock = _pLastBlock;
    pNewBlock->_blockSize = newBlockSize;

    if (_pLastBlock != nullptr) {
      _totalCapacity += newBlockSize;
    }
    _pLastBlock = pNewBlock;
    _pNextSlot = slotBegin(pNewBlock);
  }

  [[nodiscard]] static Slot* slotFromObject(T* pObj) noexcept {
    static_assert(std::is_standard_layout_v<Slot>);

    static constexpr size_type kStorageOffset = offsetof(Slot, storage);

    return reinterpret_cast<Slot*>(reinterpret_cast<std::byte*>(pObj) - kStorageOffset);
  }

  Block* _pLastBlock{nullptr};
  Slot* _pFreeList{nullptr};
  Slot* _pNextSlot{nullptr};
  // totalCapacity tracks the total number of allocated slots in the pool
  // or the initial capacity if no blocks have been allocated yet.
  size_type _totalCapacity{kDefaultInitialCapacity};
  size_type _liveCount{0};
};

template <class T, class SizeType>
ObjectPool<T, SizeType>::ObjectPool(ObjectPool&& other) noexcept
    : _pLastBlock(std::exchange(other._pLastBlock, nullptr)),
      _pFreeList(std::exchange(other._pFreeList, nullptr)),
      _pNextSlot(std::exchange(other._pNextSlot, nullptr)),
      _totalCapacity(std::exchange(other._totalCapacity, kDefaultInitialCapacity)),
      _liveCount(std::exchange(other._liveCount, 0)) {}

template <class T, class SizeType>
ObjectPool<T, SizeType>& ObjectPool<T, SizeType>::operator=(ObjectPool&& other) noexcept {
  if (this != &other) {
    reset();

    _pLastBlock = std::exchange(other._pLastBlock, nullptr);
    _pFreeList = std::exchange(other._pFreeList, nullptr);
    _pNextSlot = std::exchange(other._pNextSlot, nullptr);
    _totalCapacity = std::exchange(other._totalCapacity, kDefaultInitialCapacity);
    _liveCount = std::exchange(other._liveCount, 0);
  }
  return *this;
}

template <class T, class SizeType>
template <class... Args>
T* ObjectPool<T, SizeType>::allocateAndConstruct(Args&&... args) {
  Slot* pSlot;
  if (_pFreeList == nullptr) {
    if (_pLastBlock == nullptr || _pNextSlot == slotEnd(_pLastBlock)) {
      addBlock();
    }

    pSlot = _pNextSlot;
    ++_pNextSlot;
  } else {
    pSlot = _pFreeList;
    _pFreeList = pSlot->nextFree();
  }

  try {
    ::new (pSlot) Slot(std::forward<Args>(args)...);
  } catch (...) {
    pSlot->setFree(_pFreeList);
    _pFreeList = pSlot;
    throw;
  }

  ++_liveCount;

  return pSlot->ptr();
}

template <class T, class SizeType>
void ObjectPool<T, SizeType>::destroyAndRelease(T* pObj) noexcept {
  Slot* pSlot = slotFromObject(pObj);

  pSlot->setFree(_pFreeList);
  _pFreeList = pSlot;
  --_liveCount;
}

template <class T, class SizeType>
T ObjectPool<T, SizeType>::release(T* pObj) noexcept
  requires std::is_move_constructible_v<T>
{
  Slot* pSlot = slotFromObject(pObj);

  T ret(std::move(*pObj));
  pSlot->setFree(_pFreeList);
  _pFreeList = pSlot;
  --_liveCount;
  return ret;
}

template <class T, class SizeType>
void ObjectPool<T, SizeType>::clear() noexcept {
  _pFreeList = nullptr;

  for (Block* pBlock = _pLastBlock; pBlock != nullptr; pBlock = pBlock->_pPrevBlock) {
    const auto nbElems =
        pBlock == _pLastBlock ? static_cast<size_type>(_pNextSlot - slotBegin(pBlock)) : pBlock->_blockSize;
    for (size_type pos = 0; pos < nbElems; ++pos) {
      Slot* pSlot = slotBegin(pBlock) + pos;
      pSlot->setFree(_pFreeList);
      _pFreeList = pSlot;
    }
  }
  _liveCount = 0U;
}

template <class T, class SizeType>
void ObjectPool<T, SizeType>::reset() noexcept {
  for (Block* pBlock = _pLastBlock; pBlock != nullptr;) {
    Block* pPrev = pBlock->_pPrevBlock;
    if constexpr (!std::is_trivially_destructible_v<T>) {
      if (pBlock == _pLastBlock) {
        std::destroy(slotBegin(pBlock), _pNextSlot);
      } else {
        std::destroy_n(slotBegin(pBlock), pBlock->_blockSize);
      }
    }

    if (pPrev == nullptr) {
      // Resets total capacity to its initial value (at construction of the pool)
      _totalCapacity = pBlock->_blockSize;
    }

    std::free(pBlock);

    pBlock = pPrev;
  }

  _pLastBlock = nullptr;
  _pFreeList = nullptr;
  _liveCount = 0U;
  _pNextSlot = nullptr;
}

/// RAII smart pointer that returns the managed object to its ObjectPool on destruction.
/// Move-only, analogous to std::unique_ptr but backed by pool allocation.
template <class T, class SizeType = uint32_t>
class PoolPtr {
 public:
  PoolPtr() noexcept = default;

  PoolPtr(std::nullptr_t) noexcept {}

  PoolPtr(ObjectPool<T, SizeType>& pool, T* pObj) noexcept : _pPool(&pool), _pObj(pObj) {}

  PoolPtr(const PoolPtr&) = delete;
  PoolPtr& operator=(const PoolPtr&) = delete;

  PoolPtr(PoolPtr&& other) noexcept
      : _pPool(std::exchange(other._pPool, nullptr)), _pObj(std::exchange(other._pObj, nullptr)) {}

  PoolPtr& operator=(PoolPtr&& other) noexcept {
    if (this != &other) [[likely]] {
      reset();

      _pPool = std::exchange(other._pPool, nullptr);
      _pObj = std::exchange(other._pObj, nullptr);
    }
    return *this;
  }

  ~PoolPtr() { reset(); }

  void reset() noexcept {
    if (_pObj != nullptr) {
      _pPool->destroyAndRelease(_pObj);
      _pObj = nullptr;
    }
  }

  [[nodiscard]] T* get() const noexcept { return _pObj; }

  T& operator*() const noexcept { return *_pObj; }

  T* operator->() const noexcept { return _pObj; }

  explicit operator bool() const noexcept { return _pObj != nullptr; }

 private:
  ObjectPool<T, SizeType>* _pPool{nullptr};
  T* _pObj{nullptr};
};

}  // namespace aeronet