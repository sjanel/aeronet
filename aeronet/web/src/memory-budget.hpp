#pragma once

#include <cstddef>

namespace aeronet::internal {

// The native memory that the scripts make a page hold (the DOM content they add, the output of document.write(),
// their timers), bounded by WebClientConfig::maxScriptMemory like their JavaScript heap. Their bindings check that the
// page can hold what they are about to allocate, then charge what they allocated. Reaching the limit is an out of
// memory error of the script (JS_ThrowOutOfMemory()).
class MemoryBudget {
 public:
  explicit MemoryBudget(std::size_t limit) noexcept : _limit(limit) {}

  // Whether the page can hold 'bytes' more.
  [[nodiscard]] bool canHold(std::size_t bytes) const noexcept { return _used <= _limit && bytes <= _limit - _used; }

  // Counts 'bytes', also beyond the limit (an estimate given to canHold() was exceeded, see exceeded()).
  void charge(std::size_t bytes) noexcept { _used += bytes; }

  // Whether the counted memory is beyond the limit.
  [[nodiscard]] bool exceeded() const noexcept { return _used > _limit; }

  // Gives back up to 'bytes' of the counted memory. Saturating: the scripts also shrink or remove the content of the
  // parsed document (attributes, text), which was never counted.
  void release(std::size_t bytes) noexcept { _used = bytes < _used ? _used - bytes : 0; }

 private:
  std::size_t _used{};
  std::size_t _limit;
};

}  // namespace aeronet::internal
