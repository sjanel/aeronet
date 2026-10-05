#pragma once

#include <atomic>

namespace aeronet {

// Integer updated by a single thread (typically a server event loop) and readable from any other thread (e.g. a
// stats() call). Updates are relaxed loads and stores rather than read-modify-write operations: as cheap as a plain
// integer on common architectures, but without the data race a plain integer would have with concurrent readers.
// Concurrent updates from several threads are NOT supported (increments could be lost).
template <class T>
class SingleWriterCounter {
 public:
  SingleWriterCounter() noexcept = default;

  // Copies (and moves) take a snapshot of the current value: they are only meant to be made while no thread updates
  // 'other'.
  SingleWriterCounter(const SingleWriterCounter& other) noexcept : _value(other.load()) {}
  SingleWriterCounter& operator=(const SingleWriterCounter& other) noexcept {
    _value.store(other.load(), std::memory_order_relaxed);
    return *this;
  }

  ~SingleWriterCounter() = default;

  SingleWriterCounter& operator=(T value) noexcept {
    _value.store(value, std::memory_order_relaxed);
    return *this;
  }

  SingleWriterCounter& operator++() noexcept { return *this += T{1}; }

  SingleWriterCounter& operator+=(T delta) noexcept {
    _value.store(load() + delta, std::memory_order_relaxed);
    return *this;
  }

  // Keeps the maximum of the current value and the given one.
  void updateMax(T value) noexcept {
    if (load() < value) {
      _value.store(value, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] T load() const noexcept { return _value.load(std::memory_order_relaxed); }

 private:
  std::atomic<T> _value{};
};

}  // namespace aeronet
