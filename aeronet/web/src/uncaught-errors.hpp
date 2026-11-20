#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "aeronet/object-array-pool.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

// The errors that no script of a page caught (see WebPage::uncaughtErrors()): the first kMaxCount, each cut after
// kMaxSize bytes, their texts in a pool of characters.
class UncaughtErrors {
 public:
  static constexpr std::size_t kMaxCount = 100;
  // The longest text kept, its stack included, before its "..." suffix.
  static constexpr std::size_t kMaxSize = 8192;

  // Keeps a copy of 'text' (valid UTF-8), cut after kMaxSize bytes at the start of a UTF-8 sequence, unless kMaxCount
  // are kept. Throws std::bad_alloc on memory exhaustion.
  void add(std::string_view text);

  [[nodiscard]] std::span<const std::string_view> texts() const noexcept { return _texts; }

 private:
  // The first block holds a few short errors.
  static constexpr ObjectArrayPool<char>::size_type kFirstBlockSize = 1024;

  ObjectArrayPool<char> _chars{kFirstBlockSize};
  vector<std::string_view> _texts;
};

}  // namespace aeronet::internal
