#pragma once

#include <cassert>
#include <cstddef>
#include <iterator>
#include <string_view>

#include "raw-bytes.hpp"
#include "string-equal-ignore-case.hpp"

namespace aeronet {

template <const char* Sep, bool ContainsCaseInsensitive = false, class SizeType = std::size_t>
class DynamicConcatenatedStrings {
 private:
  static constexpr char kNullChar = '\0';
  static constexpr std::string_view kSep = Sep[0] == '\0' ? std::string_view(&kNullChar, 1UL) : std::string_view(Sep);

 public:
  using size_type = SizeType;
  using BufferType = RawBytesImpl<char, std::string_view, SizeType>;

  DynamicConcatenatedStrings() noexcept = default;

  explicit DynamicConcatenatedStrings(size_type initialCapacity) : _buf(initialCapacity) {}

  // Append a new string part.
  // The string must not contain the separator character.
  void append(std::string_view str) {
    assert(!str.contains(kSep));
    _buf.ensureAvailableCapacity(static_cast<size_type>(str.size() + kSep.size()));
    _buf.unchecked_append(str);
    _buf.unchecked_append(kSep);
  }

  // Check whether a given part is already contained.
  [[nodiscard]] bool contains(std::string_view part) const noexcept {
    std::string_view buf = _buf;
    while (!buf.empty()) {
      const auto nextSep = buf.find(kSep);
      std::string_view currentPart{buf.substr(0, nextSep)};
      if constexpr (ContainsCaseInsensitive) {
        if (CaseInsensitiveEqual(currentPart, part)) {
          return true;
        }
      } else {
        if (currentPart == part) {
          return true;
        }
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

    reference operator*() const noexcept { return _cur; }
    pointer operator->() const noexcept { return &_cur; }

    iterator& operator++() noexcept {
      advance();
      return *this;
    }

    iterator operator++(int) noexcept {
      iterator temp = *this;
      advance();
      return temp;
    }

    bool operator==(const iterator&) const noexcept = default;

   private:
    friend class DynamicConcatenatedStrings<Sep, ContainsCaseInsensitive, SizeType>;

    explicit iterator(std::string_view buf) noexcept : _cur(buf.begin(), buf.end()), _end(buf.end()) { advance(true); }

    void advance(bool init = false) noexcept {
      if ((init && _cur.empty()) || _cur.end() + kSep.size() == _end) {
        _cur = std::string_view{};
        _end = nullptr;
        return;
      }
      for (const char* endPtr = init ? _cur.begin() : _cur.end() + kSep.size(); endPtr != _end; ++endPtr) {
        if (std::string_view(endPtr, kSep.size()) == kSep) {
          if (init) {
            _cur = std::string_view(_cur.begin(), endPtr);
          } else {
            _cur = std::string_view(_cur.end() + kSep.size(), endPtr);
          }

          return;
        }
      }
    }

    std::string_view _cur;
    const char* _end = nullptr;
  };

  // Range helpers
  [[nodiscard]] iterator begin() const noexcept { return iterator(std::string_view(_buf.data(), _buf.size())); }
  [[nodiscard]] iterator end() const noexcept { return {}; }

  // Get the full concatenated string
  [[nodiscard]] std::string_view fullString(bool removeLastSep = true) const noexcept {
    std::string_view ret(_buf.data(), _buf.size());
    if (removeLastSep && !_buf.empty()) {
      ret.remove_suffix(kSep.size());
    }
    return ret;
  }

  // Get the full size of the concatenated string
  [[nodiscard]] size_type fullSize(bool removeLastSep = true) const noexcept {
    size_type ret = _buf.size();
    if (removeLastSep && !_buf.empty()) {
      ret -= kSep.size();
    }
    return ret;
  }

  // Check if there are no concatenated strings
  [[nodiscard]] bool empty() const noexcept { return _buf.empty(); }

  // Clear all concatenated strings
  void clear() noexcept { _buf.clear(); }

  // Get the current capacity of the internal buffer
  [[nodiscard]] size_type capacity() const noexcept { return _buf.capacity(); }

  // Get the number of concatenated strings.
  // To get the full size of the concatenated string, use fullSize().
  [[nodiscard]] size_type size() const noexcept {
    size_type count = 0;
    for (std::string_view buf = _buf; !buf.empty(); ++count) {
      const auto nextSep = buf.find(kSep);
      buf.remove_prefix(nextSep + kSep.size());
    }
    return count;
  }

  // Capture the full concatenated string buffer (moves it out).
  [[nodiscard]] BufferType captureFullString(bool removeLastSep = true) noexcept {
    auto ret = std::move(_buf);
    if (removeLastSep && !ret.empty()) {
      ret.setSize(ret.size() - kSep.size());
    }
    return ret;
  }

  bool operator==(const DynamicConcatenatedStrings& other) const noexcept = default;

 private:
  BufferType _buf;
};

}  // namespace aeronet