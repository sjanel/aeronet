#pragma once

#include <cassert>
#include <cstddef>
#include <string_view>

#include "raw-chars.hpp"
#include "string-equal-ignore-case.hpp"

namespace aeronet {

template <const char *Sep, bool ContainsCaseInsensitive = false>
class DynamicConcatenatedStrings {
 private:
  static constexpr char kNullChar = '\0';
  static constexpr std::string_view kSep = Sep[0] == '\0' ? std::string_view(&kNullChar, 1UL) : std::string_view(Sep);

 public:
  DynamicConcatenatedStrings() noexcept = default;

  explicit DynamicConcatenatedStrings(std::size_t initialCapacity) : _buf(initialCapacity) {}

  // Append a new string part.
  // The string must not contain the separator character.
  void append(std::string_view str) {
    assert(!str.contains(kSep));
    _buf.ensureAvailableCapacity(str.size() + kSep.size());
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

  // Get the full concatenated string
  [[nodiscard]] std::string_view fullString(bool removeLastSep = true) const noexcept {
    std::string_view ret(_buf.data(), _buf.size());
    if (removeLastSep && !_buf.empty()) {
      ret.remove_suffix(kSep.size());
    }
    return ret;
  }

  // Get the full size of the concatenated string
  [[nodiscard]] std::size_t fullSize(bool removeLastSep = true) const noexcept {
    auto ret = _buf.size();
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
  [[nodiscard]] std::size_t capacity() const noexcept { return _buf.capacity(); }

  // Get the number of concatenated strings.
  // To get the full size of the concatenated string, use fullSize().
  [[nodiscard]] std::size_t size() const noexcept {
    std::size_t count = 0;
    for (std::string_view buf = _buf; !buf.empty(); ++count) {
      const auto nextSep = buf.find(kSep);
      buf.remove_prefix(nextSep + kSep.size());
    }
    return count;
  }

  // Capture the full concatenated string buffer (moves it out).
  [[nodiscard]] RawChars captureFullString(bool removeLastSep = true) noexcept {
    auto ret = std::move(_buf);
    if (removeLastSep && !ret.empty()) {
      ret.setSize(ret.size() - kSep.size());
    }
    return ret;
  }

 private:
  RawChars _buf;
};

}  // namespace aeronet