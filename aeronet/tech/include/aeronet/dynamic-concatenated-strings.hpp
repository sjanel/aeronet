#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <type_traits>

#include "aeronet/internal/raw-bytes-base.hpp"
#include "aeronet/string-equal-ignore-case.hpp"

namespace aeronet {

template <const char* Sep, class SizeType = uint64_t>
class DynamicConcatenatedStrings {
 private:
  static_assert(std::is_unsigned_v<SizeType>);

  static constexpr char kNullChar = '\0';

 public:
  static constexpr std::string_view kSep = Sep[0] == '\0' ? std::string_view(&kNullChar, 1UL) : std::string_view(Sep);

  static_assert(!kSep.empty());

  using size_type = SizeType;
  using BufferType = RawBytesBase<char, std::string_view, SizeType>;

  DynamicConcatenatedStrings() noexcept = default;

  explicit DynamicConcatenatedStrings(size_type initialCapacity) : _buf(initialCapacity) {}

  // Append a new string part.
  // The string must not contain the separator character.
  void append(std::string_view str) {
    _buf.ensureAvailableCapacityExponential(str.size() + kSep.size());
    assert(!str.contains(kSep));
    _buf.unchecked_append(str);
    _buf.unchecked_append(kSep);
  }

  // Check whether a given part is already contained.
  [[nodiscard]] bool contains(std::string_view part) const noexcept {
    const std::string_view buf = _buf;
    const auto pos = buf.find(part);
    if (pos == std::string_view::npos || buf.substr(pos + part.size(), kSep.size()) != kSep) {
      return false;
    }
    return pos == 0 || (pos > kSep.size() && buf.substr(pos - kSep.size(), kSep.size()) == kSep);
  }

  // Check whether a given part is already contained (case-insensitive).
  [[nodiscard]] bool containsCI(std::string_view part) const noexcept {
    std::string_view buf = _buf;
    while (!buf.empty()) {
      const auto nextSep = buf.find(kSep);
      const std::string_view currentPart{buf.begin(), nextSep};
      if (CaseInsensitiveEqual(currentPart, part)) {
        return true;
      }
      buf.remove_prefix(nextSep + kSep.size());
    }
    return false;
  }

  // Non-allocating forward iterator over the concatenated parts. Yields std::string_view for each part.
  class iterator {
   public:
    using value_type = std::string_view;
    using reference = std::string_view;
    using pointer = const std::string_view*;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    iterator() noexcept = default;

    auto operator*() const noexcept { return _cur; }
    auto operator->() const noexcept { return &_cur; }

    iterator& operator++() noexcept {
      advance();
      return *this;
    }

    iterator operator++(int) noexcept {
      iterator tmp = *this;
      advance();
      return tmp;
    }

    bool operator==(const iterator&) const noexcept = default;

   private:
    friend class DynamicConcatenatedStrings<Sep, SizeType>;

    iterator(const char* first, const char* last) noexcept : _cur(first, last), _end(last) { advance(true); }

    void advance(bool init = false) noexcept {
      if ((init && _cur.empty()) || _cur.end() + kSep.size() == _end) {
        _cur = {};
        _end = nullptr;
      } else {
        const char* begPtr = init ? _cur.begin() : _cur.end() + kSep.size();
        const auto sepPos = std::string_view(begPtr, _end).find(kSep);
        assert(sepPos != std::string_view::npos);
        _cur = std::string_view(begPtr, begPtr + sepPos);
      }
    }

    std::string_view _cur;
    const char* _end = nullptr;
  };

  // Range helpers
  [[nodiscard]] iterator begin() const noexcept { return iterator(_buf.data(), _buf.data() + _buf.size()); }
  [[nodiscard]] iterator end() const noexcept { return {}; }

  // Get the full concatenated string
  // So if there are N elements, it will be size of all elements plus (N-1) * sep.size()
  [[nodiscard]] std::string_view fullString() const noexcept { return {_buf.data(), fullSize()}; }

  // Get the full concatenated string
  // Includes the last separator (so N elements + N separators)
  [[nodiscard]] std::string_view fullStringWithLastSep() const noexcept { return {_buf.data(), fullSizeWithLastSep()}; }

  // Get the full size of the concatenated string without the last separator
  // So if there are N elements, it will be size of all elements plus (N-1) * sep.size()
  [[nodiscard]] size_type fullSize() const noexcept {
    return _buf.size() == 0 ? 0 : (_buf.size() - static_cast<size_type>(kSep.size()));
  }

  // Get the full size of the concatenated string
  // Includes the last separator (so N elements + N separators)
  [[nodiscard]] size_type fullSizeWithLastSep() const noexcept { return _buf.size(); }

  // Check if there are no concatenated strings
  [[nodiscard]] bool empty() const noexcept { return _buf.empty(); }

  // Clear all concatenated strings
  void clear() noexcept { _buf.clear(); }

  // Get the number of concatenated strings
  // The complexity is linear in the number of concatenated strings.
  [[nodiscard]] size_type nbConcatenatedStrings() const noexcept {
    size_type count = 0;
    std::string_view buf = _buf;
    for (std::string_view::size_type pos = 0; pos < buf.size(); ++count) {
      pos = buf.find(kSep, pos) + kSep.size();
    }
    return count;
  }

  // Get the current capacity of the internal buffer
  [[nodiscard]] size_type internalBufferCapacity() const noexcept { return _buf.capacity(); }

  bool operator==(const DynamicConcatenatedStrings& other) const noexcept = default;

  using trivially_relocatable = std::true_type;

 private:
  BufferType _buf;
};

}  // namespace aeronet